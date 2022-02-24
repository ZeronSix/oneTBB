/*
    Copyright (c) 2005-2022 Intel Corporation

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include "oneapi/tbb/global_control.h" // global_control::active_value

#include "thread_dispatcher.h"
#include "market.h"
#include "main.h"
#include "governor.h"
#include "arena.h"
#include "thread_data.h"
#include "itt_notify.h"

#include <cstring> // std::memset()

#include "clients.h"

namespace tbb {
namespace detail {
namespace r1 {


struct tbb_permit_manager_client : public permit_manager_client, public d1::intrusive_list_node {
    tbb_permit_manager_client(arena& a, thread_dispatcher& td) : permit_manager_client(a, td) {}

    void update_allotment() override {
        unsigned prev_allotment = my_arena.exchange_allotment(my_num_workers_allotted);
        int delta = my_num_workers_allotted - prev_allotment;

        suppress_unused_warning(delta);
    }

    void set_allotment(unsigned allotment) {
        my_num_workers_allotted = allotment;
    }

    // arena needs an extra worker despite a global limit
    std::atomic<bool> m_global_concurrency_mode{false};

    //! The index in the array of per priority lists of arenas this object is in.
    unsigned priority_level() {
        return my_arena.priority_level();
    }

    bool has_enqueued_tasks() {
        return my_arena.has_enqueued_tasks();
    }

    std::uintptr_t aba_epoch() {
        return my_arena.aba_epoch();
    }

    int num_workers_requested() {
        return my_arena.num_workers_requested();
    }

    unsigned references() {
        return my_arena.references();
    }

    thread_pool_ticket& ticket() {
        return my_ticket;
    }

    void set_top_priority(bool b) {
        return my_is_top_priority.store(b, std::memory_order_relaxed);
    }

    unsigned my_num_workers_allotted;
};

void market::insert_arena_into_list (tbb_permit_manager_client& a ) {
    __TBB_ASSERT( a.priority_level() < num_priority_levels, nullptr );
    my_arenas[a.priority_level()].push_front( a );
}

void market::remove_arena_from_list (tbb_permit_manager_client& a ) {
    __TBB_ASSERT( a.priority_level() < num_priority_levels, nullptr );
    my_arenas[a.priority_level()].remove( a );
}

//------------------------------------------------------------------------
// market
//------------------------------------------------------------------------

market::market ( unsigned workers_soft_limit, unsigned workers_hard_limit, std::size_t stack_size )
    : my_num_workers_soft_limit(workers_soft_limit)
    , my_next_arena(nullptr)
    , my_ref_count(1)
    , my_workers_soft_limit_to_report(workers_soft_limit)
{
    // Once created RML server will start initializing workers that will need
    // global market instance to get worker stack size
    my_thread_dispatcher = new thread_dispatcher(*this, workers_hard_limit, stack_size);
}

market::~market() {
    poison_pointer(my_next_arena);
}

std::size_t market::worker_stack_size() const { return my_thread_dispatcher->my_stack_size; }

unsigned market::max_num_workers() {
    global_market_mutex_type::scoped_lock lock( theMarketMutex );
    return theMarket? theMarket->my_thread_dispatcher->my_num_workers_hard_limit : 0;
}

static unsigned calc_workers_soft_limit(unsigned workers_soft_limit, unsigned workers_hard_limit) {
    if( int soft_limit = market::app_parallelism_limit() )
        workers_soft_limit = soft_limit-1;
    else // if user set no limits (yet), use market's parameter
        workers_soft_limit = max( governor::default_num_threads() - 1, workers_soft_limit );
    if( workers_soft_limit >= workers_hard_limit )
        workers_soft_limit = workers_hard_limit-1;
    return workers_soft_limit;
}

bool market::add_ref_unsafe( global_market_mutex_type::scoped_lock& lock, bool is_public, unsigned workers_requested, std::size_t stack_size ) {
    market *m = theMarket;
    if( m ) {
        ++m->my_ref_count;
        const unsigned old_public_count = is_public ? m->my_public_ref_count++ : /*any non-zero value*/1;
        lock.release();
        if( old_public_count==0 )
            set_active_num_workers( calc_workers_soft_limit(workers_requested, m->my_thread_dispatcher->my_num_workers_hard_limit) );

        // do not warn if default number of workers is requested
        if( workers_requested != governor::default_num_threads()-1 ) {
            __TBB_ASSERT( skip_soft_limit_warning > workers_requested,
                          "skip_soft_limit_warning must be larger than any valid workers_requested" );
            unsigned soft_limit_to_report = m->my_workers_soft_limit_to_report.load(std::memory_order_relaxed);
            if( soft_limit_to_report < workers_requested ) {
                runtime_warning( "The number of workers is currently limited to %u. "
                                 "The request for %u workers is ignored. Further requests for more workers "
                                 "will be silently ignored until the limit changes.\n",
                                 soft_limit_to_report, workers_requested );
                // The race is possible when multiple threads report warnings.
                // We are OK with that, as there are just multiple warnings.
                unsigned expected_limit = soft_limit_to_report;
                m->my_workers_soft_limit_to_report.compare_exchange_strong(expected_limit, skip_soft_limit_warning);
            }

        }
        if( m->my_thread_dispatcher->my_stack_size < stack_size )
            runtime_warning( "Thread stack size has been already set to %u. "
                             "The request for larger stack (%u) cannot be satisfied.\n", m->my_thread_dispatcher->my_stack_size, stack_size );
        return true;
    }
    return false;
}

market& market::global_market(bool is_public, unsigned workers_requested, std::size_t stack_size) {
    global_market_mutex_type::scoped_lock lock( theMarketMutex );
    if( !market::add_ref_unsafe(lock, is_public, workers_requested, stack_size) ) {
        // TODO: A lot is done under theMarketMutex locked. Can anything be moved out?
        if( stack_size == 0 )
            stack_size = global_control::active_value(global_control::thread_stack_size);
        // Expecting that 4P is suitable for most applications.
        // Limit to 2P for large thread number.
        // TODO: ask RML for max concurrency and possibly correct hard_limit
        const unsigned factor = governor::default_num_threads()<=128? 4 : 2;
        // The requested number of threads is intentionally not considered in
        // computation of the hard limit, in order to separate responsibilities
        // and avoid complicated interactions between global_control and task_scheduler_init.
        // The market guarantees that at least 256 threads might be created.
        const unsigned workers_hard_limit = max(max(factor*governor::default_num_threads(), 256u), app_parallelism_limit());
        const unsigned workers_soft_limit = calc_workers_soft_limit(workers_requested, workers_hard_limit);
        // Create the global market instance
        std::size_t size = sizeof(market);
        __TBB_ASSERT( __TBB_offsetof(market, my_workers) + sizeof(std::atomic<thread_data*>) == sizeof(market),
                      "my_workers must be the last data field of the market class");
        size += sizeof(std::atomic<thread_data*>) * (workers_hard_limit - 1);
        __TBB_InitOnce::add_ref();
        void* storage = cache_aligned_allocate(size);
        std::memset( storage, 0, size );
        // Initialize and publish global market
        market* m = new (storage) market( workers_soft_limit, workers_hard_limit, stack_size );
        if( is_public )
            m->my_public_ref_count.store(1, std::memory_order_relaxed);
        if (market::is_lifetime_control_present()) {
            ++m->my_public_ref_count;
            ++m->my_ref_count;
        }
        theMarket = m;
        // This check relies on the fact that for shared RML default_concurrency==max_concurrency
        if ( !governor::UsePrivateRML && m->my_thread_dispatcher->my_server->default_concurrency() < workers_soft_limit )
            runtime_warning( "RML might limit the number of workers to %u while %u is requested.\n"
                    , m->my_thread_dispatcher->my_server->default_concurrency(), workers_soft_limit );
    }
    return *theMarket;
}

void market::destroy () {
    delete my_thread_dispatcher;
    this->market::~market(); // qualified to suppress warning
    cache_aligned_deallocate( this );
    __TBB_InitOnce::remove_ref();
}

bool market::release ( bool is_public, bool blocking_terminate ) {
    market::enforce([this] { return theMarket == this; }, "Global market instance was destroyed prematurely?");
    bool do_release = false;
    {
        global_market_mutex_type::scoped_lock lock( theMarketMutex );
        if ( blocking_terminate ) {
            __TBB_ASSERT( is_public, "Only an object with a public reference can request the blocking terminate" );
            while ( my_public_ref_count.load(std::memory_order_relaxed) == 1 &&
                    my_ref_count.load(std::memory_order_relaxed) > 1 ) {
                lock.release();
                // To guarantee that request_close_connection() is called by the last external thread, we need to wait till all
                // references are released. Re-read my_public_ref_count to limit waiting if new external threads are created.
                // Theoretically, new private references to the market can be added during waiting making it potentially
                // endless.
                // TODO: revise why the weak scheduler needs market's pointer and try to remove this wait.
                // Note that the market should know about its schedulers for cancellation/exception/priority propagation,
                // see e.g. task_group_context::cancel_group_execution()
                while ( my_public_ref_count.load(std::memory_order_acquire) == 1 &&
                        my_ref_count.load(std::memory_order_acquire) > 1 ) {
                    yield();
                }
                lock.acquire( theMarketMutex );
            }
        }
        if ( is_public ) {
            __TBB_ASSERT( theMarket == this, "Global market instance was destroyed prematurely?" );
            __TBB_ASSERT( my_public_ref_count.load(std::memory_order_relaxed), nullptr);
            --my_public_ref_count;
        }
        if ( --my_ref_count == 0 ) {
            __TBB_ASSERT( !my_public_ref_count.load(std::memory_order_relaxed), nullptr);
            do_release = true;
            theMarket = nullptr;
        }
    }
    if( do_release ) {
        __TBB_ASSERT( !my_public_ref_count.load(std::memory_order_relaxed),
            "No public references remain if we remove the market." );
        // inform RML that blocking termination is required
        my_thread_dispatcher->my_join_workers = blocking_terminate;
        my_thread_dispatcher->my_server->request_close_connection();
        return blocking_terminate;
    }
    return false;
}

int market::update_workers_request() {
    int old_request = my_num_workers_requested;
    my_num_workers_requested = min(my_total_demand.load(std::memory_order_relaxed),
                                   (int)my_num_workers_soft_limit.load(std::memory_order_relaxed));
#if __TBB_ENQUEUE_ENFORCED_CONCURRENCY
    if (my_mandatory_num_requested > 0) {
        __TBB_ASSERT(my_num_workers_soft_limit.load(std::memory_order_relaxed) == 0, nullptr);
        my_num_workers_requested = 1;
    }
#endif
    update_allotment(my_num_workers_requested);
    return my_num_workers_requested - old_request;
}

void market::set_active_num_workers ( unsigned soft_limit ) {
    market *m;

    {
        global_market_mutex_type::scoped_lock lock( theMarketMutex );
        if ( !theMarket )
            return; // actual value will be used at market creation
        m = theMarket;
        if (m->my_num_workers_soft_limit.load(std::memory_order_relaxed) == soft_limit)
            return;
        ++m->my_ref_count;
    }
    // have my_ref_count for market, use it safely

    int delta = 0;
    {
        arenas_list_mutex_type::scoped_lock lock( m->my_arenas_list_mutex );
        __TBB_ASSERT(soft_limit <= m->my_thread_dispatcher->my_num_workers_hard_limit, nullptr);

#if __TBB_ENQUEUE_ENFORCED_CONCURRENCY
        arena_list_type* arenas = m->my_arenas;

        if (m->my_num_workers_soft_limit.load(std::memory_order_relaxed) == 0 &&
            m->my_mandatory_num_requested > 0)
        {
            for (unsigned level = 0; level < num_priority_levels; ++level )
                for (arena_list_type::iterator it = arenas[level].begin(); it != arenas[level].end(); ++it)
                    if (it->m_global_concurrency_mode.load(std::memory_order_relaxed))
                        m->disable_mandatory_concurrency_impl(&*it);
        }
        __TBB_ASSERT(m->my_mandatory_num_requested == 0, nullptr);
#endif

        m->my_num_workers_soft_limit.store(soft_limit, std::memory_order_release);
        // report only once after new soft limit value is set
        m->my_workers_soft_limit_to_report.store(soft_limit, std::memory_order_relaxed);

#if __TBB_ENQUEUE_ENFORCED_CONCURRENCY
        if (m->my_num_workers_soft_limit.load(std::memory_order_relaxed) == 0) {
            for (unsigned level = 0; level < num_priority_levels; ++level )
                for (arena_list_type::iterator it = arenas[level].begin(); it != arenas[level].end(); ++it)
                    if (it->has_enqueued_tasks())
                        m->enable_mandatory_concurrency_impl(&*it);
        }
#endif

        delta = m->update_workers_request();
    }
    // adjust_job_count_estimate must be called outside of any locks
    if( delta!=0 )
        m->my_thread_dispatcher->my_server->adjust_job_count_estimate( delta );
    // release internal market reference to match ++m->my_ref_count above
    m->release( /*is_public=*/false, /*blocking_terminate=*/false );
}

bool market::propagate_task_group_state(std::atomic<std::uint32_t> d1::task_group_context::* mptr_state, d1::task_group_context& src, std::uint32_t new_state) {
    if (src.my_may_have_children.load(std::memory_order_relaxed) != d1::task_group_context::may_have_children)
        return true;
    // The whole propagation algorithm is under the lock in order to ensure correctness
    // in case of concurrent state changes at the different levels of the context tree.
    // See comment at the bottom of scheduler.cpp
    context_state_propagation_mutex_type::scoped_lock lock(the_context_state_propagation_mutex);
    if ((src.*mptr_state).load(std::memory_order_relaxed) != new_state)
        // Another thread has concurrently changed the state. Back down.
        return false;
    // Advance global state propagation epoch
    ++the_context_state_propagation_epoch;
    // Propagate to all workers and external threads and sync up their local epochs with the global one
    unsigned num_workers = my_thread_dispatcher->my_first_unused_worker_idx;
    for (unsigned i = 0; i < num_workers; ++i) {
        thread_data* td = my_workers[i].load(std::memory_order_acquire);
        // If the worker is only about to be registered, skip it.
        if (td)
            td->propagate_task_group_state(mptr_state, src, new_state);
    }
    // Propagate to all external threads
    // The whole propagation sequence is locked, thus no contention is expected
    for (thread_data_list_type::iterator it = my_masters.begin(); it != my_masters.end(); it++)
        it->propagate_task_group_state(mptr_state, src, new_state);
    return true;
}

permit_manager_client* market::create_client(arena& a, constraits_type*) {
    auto c = new tbb_permit_manager_client(a, *my_thread_dispatcher);
    // Add newly created arena into the existing market's list.
    arenas_list_mutex_type::scoped_lock lock(my_arenas_list_mutex);
    insert_arena_into_list(*c);
    my_thread_dispatcher->insert_ticket(c->ticket());
    return c;
}

void market::destroy_client(permit_manager_client& c) {
    delete &c;
}

void market::request_demand(unsigned min, unsigned max, permit_manager_client&) {
    suppress_unused_warning(min, max);
}

void market::release_demand(permit_manager_client&) {

}

/** This method must be invoked under my_arenas_list_mutex. **/
void market::detach_arena (tbb_permit_manager_client& a ) {
    market::enforce([this] { return theMarket == this; }, "Global market instance was destroyed prematurely?");
    //__TBB_ASSERT( !a.my_slots[0].is_occupied(), NULL );
    if (a.m_global_concurrency_mode.load(std::memory_order_relaxed))
        disable_mandatory_concurrency_impl(&a);

    remove_arena_from_list(a);
    my_thread_dispatcher->remove_ticket(a.ticket());
    if (a.aba_epoch() == my_arenas_aba_epoch.load(std::memory_order_relaxed)) {
        my_arenas_aba_epoch.store(my_arenas_aba_epoch.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
}

bool market::try_destroy_arena (permit_manager_client* c, uintptr_t aba_epoch, unsigned priority_level ) {
    auto a = static_cast<tbb_permit_manager_client*>(c);
    bool locked = true;
    __TBB_ASSERT( a, nullptr);
    // we hold reference to the server, so market cannot be destroyed at any moment here
    __TBB_ASSERT(!is_poisoned(my_next_arena), nullptr);
    my_arenas_list_mutex.lock();
        arena_list_type::iterator it = my_arenas[priority_level].begin();
        for ( ; it != my_arenas[priority_level].end(); ++it ) {
            if ( a == &*it ) {
                if ( it->aba_epoch() == aba_epoch ) {
                    // Arena is alive
                    // Acquire my_references to sync with threads that just left the arena
                    if (!a->num_workers_requested() && !a->references()) {
                        /*__TBB_ASSERT(
                            !a->my_num_workers_allotted.load(std::memory_order_relaxed) &&
                            (a->my_pool_state == arena::SNAPSHOT_EMPTY || !a->my_max_num_workers),
                            "Inconsistent arena state"
                        );*/
                        // Arena is abandoned. Destroy it.
                        detach_arena( *a );
                        my_arenas_list_mutex.unlock();
                        locked = false;
                        //a->free_arena();
                        return true;
                    }
                }
                if (locked)
                    my_arenas_list_mutex.unlock();
                return false;
            }
        }
    my_arenas_list_mutex.unlock();
    return false;
}

int market::update_allotment ( arena_list_type* arenas, int workers_demand, int max_workers ) {
    __TBB_ASSERT( workers_demand > 0, nullptr );
    max_workers = min(workers_demand, max_workers);
    int unassigned_workers = max_workers;
    int assigned = 0;
    int carry = 0;
    unsigned max_priority_level = num_priority_levels;
    for (unsigned list_idx = 0; list_idx < num_priority_levels; ++list_idx ) {
        int assigned_per_priority = min(my_priority_level_demand[list_idx], unassigned_workers);
        unassigned_workers -= assigned_per_priority;
        for (arena_list_type::iterator it = arenas[list_idx].begin(); it != arenas[list_idx].end(); ++it) {
            tbb_permit_manager_client& a = *it;
            __TBB_ASSERT(a.num_workers_requested() >= 0, nullptr);
            //__TBB_ASSERT(a.num_workers_requested() <= int(a.my_max_num_workers)
            //    || (a.my_max_num_workers == 0 && a.my_local_concurrency_requests > 0 && a.num_workers_requested() == 1), nullptr);
            if (a.num_workers_requested() == 0) {
             //   __TBB_ASSERT(!a.my_num_workers_allotted.load(std::memory_order_relaxed), nullptr);
                continue;
            }

            if (max_priority_level == num_priority_levels) {
                max_priority_level = list_idx;
            }

            int allotted = 0;
#if __TBB_ENQUEUE_ENFORCED_CONCURRENCY
            if (my_num_workers_soft_limit.load(std::memory_order_relaxed) == 0) {
                __TBB_ASSERT(max_workers == 0 || max_workers == 1, nullptr);
                allotted = a.m_global_concurrency_mode.load(std::memory_order_relaxed) &&
                    assigned < max_workers ? 1 : 0;
            } else
#endif
            {
                int tmp = a.num_workers_requested() * assigned_per_priority + carry;
                allotted = tmp / my_priority_level_demand[list_idx];
                carry = tmp % my_priority_level_demand[list_idx];
                __TBB_ASSERT(allotted <= a.num_workers_requested(), nullptr);
                //__TBB_ASSERT(allotted <= int(a.my_num_slots - a.my_num_reserved_slots), nullptr);
            }
            //a.my_num_workers_allotted.store(allotted, std::memory_order_relaxed);
            a.set_allotment(allotted);
            a.set_top_priority(list_idx == max_priority_level);
            a.update_allotment();
            assigned += allotted;
        }
    }
    __TBB_ASSERT( 0 <= assigned && assigned <= max_workers, nullptr );
    return assigned;
}

#if __TBB_ENQUEUE_ENFORCED_CONCURRENCY
void market::enable_mandatory_concurrency_impl (tbb_permit_manager_client*a ) {
    __TBB_ASSERT(!a->m_global_concurrency_mode.load(std::memory_order_relaxed), NULL);
    __TBB_ASSERT(my_num_workers_soft_limit.load(std::memory_order_relaxed) == 0, NULL);

    a->m_global_concurrency_mode.store(true, std::memory_order_relaxed);
    my_mandatory_num_requested++;
}

bool market::is_global_concurrency_disabled(permit_manager_client *c) {
    tbb_permit_manager_client* a = static_cast<tbb_permit_manager_client*>(c);
    return my_num_workers_soft_limit.load(std::memory_order_acquire) == 0 && a->m_global_concurrency_mode.load(std::memory_order_acquire) == false;
}

void market::enable_mandatory_concurrency ( permit_manager_client *c ) {
    tbb_permit_manager_client* a = static_cast<tbb_permit_manager_client*>(c);
    if (is_global_concurrency_disabled(a)) {
        int delta = 0;
        {
            arenas_list_mutex_type::scoped_lock lock(my_arenas_list_mutex);
            if (my_num_workers_soft_limit.load(std::memory_order_relaxed) != 0 ||
                a->m_global_concurrency_mode.load(std::memory_order_relaxed))
                return;

            enable_mandatory_concurrency_impl(a);
            delta = update_workers_request();
        }

        if (delta != 0)
            my_thread_dispatcher->my_server->adjust_job_count_estimate(delta);
    }
}

void market::disable_mandatory_concurrency_impl(tbb_permit_manager_client* a) {
    __TBB_ASSERT(a->m_global_concurrency_mode.load(std::memory_order_relaxed), NULL);
    __TBB_ASSERT(my_mandatory_num_requested > 0, NULL);

    a->m_global_concurrency_mode.store(false, std::memory_order_relaxed);
    my_mandatory_num_requested--;
}

void market::mandatory_concurrency_disable (permit_manager_client*c ) {
    tbb_permit_manager_client* a = static_cast<tbb_permit_manager_client*>(c);
    if ( a->m_global_concurrency_mode.load(std::memory_order_acquire) == true ) {
        int delta = 0;
        {
            arenas_list_mutex_type::scoped_lock lock(my_arenas_list_mutex);
            if (!a->m_global_concurrency_mode.load(std::memory_order_relaxed))
                return;
            // There is a racy window in advertise_new_work between mandtory concurrency enabling and 
            // setting SNAPSHOT_FULL. It gives a chance to spawn request to disable mandatory concurrency.
            // Therefore, we double check that there is no enqueued tasks.
            if (a->has_enqueued_tasks())
                return;

            __TBB_ASSERT(my_num_workers_soft_limit.load(std::memory_order_relaxed) == 0, NULL);
            disable_mandatory_concurrency_impl(a);

            delta = update_workers_request();
        }
        if (delta != 0)
            my_thread_dispatcher->my_server->adjust_job_count_estimate(delta);
    }
}
#endif /* __TBB_ENQUEUE_ENFORCED_CONCURRENCY */

void market::adjust_demand (permit_manager_client& c, int delta, bool mandatory ) {
    auto& a = static_cast<tbb_permit_manager_client&>(c);
    if (!delta) {
        return;
    }
    int target_epoch{};
    {
        arenas_list_mutex_type::scoped_lock lock(my_arenas_list_mutex);
        __TBB_ASSERT(theMarket != nullptr, "market instance was destroyed prematurely?");

        delta = a.update_request(delta, mandatory);

        if (!delta) {
            return;
        }

        int total_demand = my_total_demand.load(std::memory_order_relaxed) + delta;
        my_total_demand.store(total_demand, std::memory_order_relaxed);
        my_priority_level_demand[a.priority_level()] += delta;
        unsigned effective_soft_limit = my_num_workers_soft_limit.load(std::memory_order_relaxed);
        if (my_mandatory_num_requested > 0) {
            __TBB_ASSERT(effective_soft_limit == 0, nullptr);
            effective_soft_limit = 1;
        }

        update_allotment(effective_soft_limit);
        if (delta > 0) {
            // can't overflow soft_limit, but remember values request by arenas in
            // my_total_demand to not prematurely release workers to RML
            if (my_num_workers_requested + delta > (int)effective_soft_limit)
                delta = effective_soft_limit - my_num_workers_requested;
        }
        else {
            // the number of workers should not be decreased below my_total_demand
            if (my_num_workers_requested + delta < total_demand)
                delta = min(total_demand, (int)effective_soft_limit) - my_num_workers_requested;
        }
        my_num_workers_requested += delta;
        __TBB_ASSERT(my_num_workers_requested <= (int)effective_soft_limit, nullptr);

        target_epoch = a.my_adjust_demand_target_epoch++;
    }

    a.my_adjust_demand_current_epoch.wait_until(target_epoch, /* context = */ target_epoch, std::memory_order_relaxed);
    // Must be called outside of any locks
    my_thread_dispatcher->my_server->adjust_job_count_estimate( delta );
    a.my_adjust_demand_current_epoch.exchange(target_epoch + 1);
    a.my_adjust_demand_current_epoch.notify_relaxed(target_epoch + 1);
}

void market::add_external_thread(thread_data& td) {
    context_state_propagation_mutex_type::scoped_lock lock(the_context_state_propagation_mutex);
    my_masters.push_front(td);
}

void market::remove_external_thread(thread_data& td) {
    context_state_propagation_mutex_type::scoped_lock lock(the_context_state_propagation_mutex);
    my_masters.remove(td);
}

} // namespace r1
} // namespace detail
} // namespace tbb
