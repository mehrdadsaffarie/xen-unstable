/******************************************************************************
 * keyhandler.c
 */

#include <asm/regs.h>
#include <xen/keyhandler.h> 
#include <xen/shutdown.h>
#include <xen/event.h>
#include <xen/console.h>
#include <xen/serial.h>
#include <xen/sched.h>
#include <xen/softirq.h>
#include <xen/domain.h>
#include <xen/rangeset.h>
#include <xen/compat.h>
#include <asm/debugger.h>
#include <asm/div64.h>

#define KEY_MAX 256
#define STR_MAX  64

static struct {
    union {
        keyhandler_t     *handler;
        irq_keyhandler_t *irq_handler;
    } u;
    unsigned int flags;
    char         desc[STR_MAX];
} key_table[KEY_MAX];

#define KEYHANDLER_IRQ_CALLBACK 0x1

static unsigned char keypress_key;

static void keypress_action(unsigned long unused)
{
    keyhandler_t *h;
    unsigned char key = keypress_key;
    console_start_log_everything();
    if ( (h = key_table[key].u.handler) != NULL )
        (*h)(key);
    console_end_log_everything();
}

static DECLARE_TASKLET(keypress_tasklet, keypress_action, 0);

void handle_keypress(unsigned char key, struct cpu_user_regs *regs)
{
    irq_keyhandler_t *h;

    if ( !in_irq() || (key_table[key].flags & KEYHANDLER_IRQ_CALLBACK) )
    {
        console_start_log_everything();
        if ( (h = key_table[key].u.irq_handler) != NULL )
            (*h)(key, regs);
        console_end_log_everything();
    }
    else
    {
        keypress_key = key;
        tasklet_schedule(&keypress_tasklet);
    }
}

void register_keyhandler(
    unsigned char key, keyhandler_t *handler, char *desc)
{
    ASSERT(key_table[key].u.handler == NULL);
    key_table[key].u.handler = handler;
    key_table[key].flags     = 0;
    safe_strcpy(key_table[key].desc, desc);
}

void register_irq_keyhandler(
    unsigned char key, irq_keyhandler_t *handler, char *desc)
{
    ASSERT(key_table[key].u.irq_handler == NULL);
    key_table[key].u.irq_handler = handler;
    key_table[key].flags         = KEYHANDLER_IRQ_CALLBACK;
    safe_strcpy(key_table[key].desc, desc);
}

static void show_handlers(unsigned char key)
{
    int i;
    printk("'%c' pressed -> showing installed handlers\n", key);
    for ( i = 0; i < KEY_MAX; i++ ) 
        if ( key_table[i].u.handler != NULL ) 
            printk(" key '%c' (ascii '%02x') => %s\n", 
                   (i<33 || i>126)?(' '):(i),i,
                   key_table[i].desc);
}

static void __dump_execstate(void *unused)
{
    dump_execution_state();
    printk("*** Dumping CPU%d guest state: ***\n", smp_processor_id());
    if ( is_idle_vcpu(current) )
        printk("No guest context (CPU is idle).\n");
    else
        show_execution_state(guest_cpu_user_regs());
}

static void dump_registers(unsigned char key, struct cpu_user_regs *regs)
{
    unsigned int cpu;

    /* We want to get everything out that we possibly can. */
    console_start_sync();

    printk("'%c' pressed -> dumping registers\n", key);

    /* Get local execution state out immediately, in case we get stuck. */
    printk("\n*** Dumping CPU%d host state: ***\n", smp_processor_id());
    __dump_execstate(NULL);

    for_each_online_cpu ( cpu )
    {
        if ( cpu == smp_processor_id() )
            continue;
        printk("\n*** Dumping CPU%d host state: ***\n", cpu);
        on_selected_cpus(cpumask_of(cpu), __dump_execstate, NULL, 1);
    }

    printk("\n");

    console_end_sync();
}

static void dump_dom0_registers(unsigned char key)
{
    struct vcpu *v;

    if ( dom0 == NULL )
        return;

    printk("'%c' pressed -> dumping Dom0's registers\n", key);

    for_each_vcpu ( dom0, v )
        vcpu_show_execution_state(v);
}

static void halt_machine(unsigned char key, struct cpu_user_regs *regs)
{
    printk("'%c' pressed -> rebooting machine\n", key);
    machine_restart(0);
}

static void cpuset_print(char *set, int size, cpumask_t mask)
{
    *set++ = '{';
    set += cpulist_scnprintf(set, size-2, mask);
    *set++ = '}';
    *set++ = '\0';
}

static void periodic_timer_print(char *str, int size, uint64_t period)
{
    if ( period == 0 )
    {
        strlcpy(str, "No periodic timer", size);
        return;
    }

    snprintf(str, size,
             "%u Hz periodic timer (period %u ms)",
             1000000000/(int)period, (int)period/1000000);
}

static void dump_domains(unsigned char key)
{
    struct domain *d;
    struct vcpu   *v;
    s_time_t       now = NOW();
    char           tmpstr[100];

    printk("'%c' pressed -> dumping domain info (now=0x%X:%08X)\n", key,
           (u32)(now>>32), (u32)now);

    rcu_read_lock(&domlist_read_lock);

    for_each_domain ( d )
    {
        printk("General information for domain %u:\n", d->domain_id);
        cpuset_print(tmpstr, sizeof(tmpstr), d->domain_dirty_cpumask);
        printk("    refcnt=%d dying=%d nr_pages=%d xenheap_pages=%d "
               "dirty_cpus=%s max_pages=%u\n",
               atomic_read(&d->refcnt), d->is_dying,
               d->tot_pages, d->xenheap_pages, tmpstr, d->max_pages);
        printk("    handle=%02x%02x%02x%02x-%02x%02x-%02x%02x-"
               "%02x%02x-%02x%02x%02x%02x%02x%02x vm_assist=%08lx\n",
               d->handle[ 0], d->handle[ 1], d->handle[ 2], d->handle[ 3],
               d->handle[ 4], d->handle[ 5], d->handle[ 6], d->handle[ 7],
               d->handle[ 8], d->handle[ 9], d->handle[10], d->handle[11],
               d->handle[12], d->handle[13], d->handle[14], d->handle[15],
               d->vm_assist);

        arch_dump_domain_info(d);

        rangeset_domain_printk(d);

        dump_pageframe_info(d);
               
        printk("VCPU information and callbacks for domain %u:\n",
               d->domain_id);
        for_each_vcpu ( d, v ) {
            printk("    VCPU%d: CPU%d [has=%c] flags=%lx poll=%d "
                   "upcall_pend = %02x, upcall_mask = %02x ",
                   v->vcpu_id, v->processor,
                   v->is_running ? 'T':'F',
                   v->pause_flags, v->poll_evtchn,
                   v->vcpu_info ? vcpu_info(v, evtchn_upcall_pending) : 0,
                   v->vcpu_info ? vcpu_info(v, evtchn_upcall_mask) : 1);
            cpuset_print(tmpstr, sizeof(tmpstr), v->vcpu_dirty_cpumask);
            printk("dirty_cpus=%s ", tmpstr);
            cpuset_print(tmpstr, sizeof(tmpstr), v->cpu_affinity);
            printk("cpu_affinity=%s\n", tmpstr);
            arch_dump_vcpu_info(v);
            periodic_timer_print(tmpstr, sizeof(tmpstr), v->periodic_period);
            printk("    %s\n", tmpstr);
            if ( !v->vcpu_info )
                continue;
            printk("    Notifying guest (virq %d, port %d, stat %d/%d/%d)\n",
                   VIRQ_DEBUG, v->virq_to_evtchn[VIRQ_DEBUG],
                   test_bit(v->virq_to_evtchn[VIRQ_DEBUG], 
                            &shared_info(d, evtchn_pending)),
                   test_bit(v->virq_to_evtchn[VIRQ_DEBUG], 
                            &shared_info(d, evtchn_mask)),
                   test_bit(v->virq_to_evtchn[VIRQ_DEBUG] /
                            BITS_PER_EVTCHN_WORD(d),
                            &vcpu_info(v, evtchn_pending_sel)));
            send_guest_vcpu_virq(v, VIRQ_DEBUG);
        }
    }

    rcu_read_unlock(&domlist_read_lock);
}

static cpumask_t read_clocks_cpumask = CPU_MASK_NONE;
static s_time_t read_clocks_time[NR_CPUS];
static u64 read_cycles_time[NR_CPUS];

static void read_clocks_slave(void *unused)
{
    unsigned int cpu = smp_processor_id();
    local_irq_disable();
    while ( !cpu_isset(cpu, read_clocks_cpumask) )
        cpu_relax();
    read_clocks_time[cpu] = NOW();
    read_cycles_time[cpu] = get_cycles();
    cpu_clear(cpu, read_clocks_cpumask);
    local_irq_enable();
}

static void read_clocks(unsigned char key)
{
    unsigned int cpu = smp_processor_id(), min_stime_cpu, max_stime_cpu;
    unsigned int min_cycles_cpu, max_cycles_cpu;
    u64 min_stime, max_stime, dif_stime;
    u64 min_cycles, max_cycles, dif_cycles;
    static u64 sumdif_stime = 0, maxdif_stime = 0;
    static u64 sumdif_cycles = 0, maxdif_cycles = 0;
    static u32 count = 0;
    static DEFINE_SPINLOCK(lock);

    spin_lock(&lock);

    smp_call_function(read_clocks_slave, NULL, 0);

    local_irq_disable();
    read_clocks_cpumask = cpu_online_map;
    read_clocks_time[cpu] = NOW();
    read_cycles_time[cpu] = get_cycles();
    cpu_clear(cpu, read_clocks_cpumask);
    local_irq_enable();

    while ( !cpus_empty(read_clocks_cpumask) )
        cpu_relax();

    min_stime_cpu = max_stime_cpu = min_cycles_cpu = max_cycles_cpu = cpu;
    for_each_online_cpu ( cpu )
    {
        if ( read_clocks_time[cpu] < read_clocks_time[min_stime_cpu] )
            min_stime_cpu = cpu;
        if ( read_clocks_time[cpu] > read_clocks_time[max_stime_cpu] )
            max_stime_cpu = cpu;
        if ( read_cycles_time[cpu] < read_cycles_time[min_cycles_cpu] )
            min_cycles_cpu = cpu;
        if ( read_cycles_time[cpu] > read_cycles_time[max_cycles_cpu] )
            max_cycles_cpu = cpu;
    }

    min_stime = read_clocks_time[min_stime_cpu];
    max_stime = read_clocks_time[max_stime_cpu];
    min_cycles = read_cycles_time[min_cycles_cpu];
    max_cycles = read_cycles_time[max_cycles_cpu];

    spin_unlock(&lock);

    dif_stime = max_stime - min_stime;
    if ( dif_stime > maxdif_stime )
        maxdif_stime = dif_stime;
    sumdif_stime += dif_stime;
    dif_cycles = max_cycles - min_cycles;
    if ( dif_cycles > maxdif_cycles )
        maxdif_cycles = dif_cycles;
    sumdif_cycles += dif_cycles;
    count++;
    printk("Synced stime skew: max=%"PRIu64"ns avg=%"PRIu64"ns "
           "samples=%"PRIu32" current=%"PRIu64"ns\n",
           maxdif_stime, sumdif_stime/count, count, dif_stime);
    printk("Synced cycles skew: max=%"PRIu64" avg=%"PRIu64" "
           "samples=%"PRIu32" current=%"PRIu64"\n",
           maxdif_cycles, sumdif_cycles/count, count, dif_cycles);
}

extern void dump_runq(unsigned char key);

#ifdef PERF_COUNTERS
extern void perfc_printall(unsigned char key);
extern void perfc_reset(unsigned char key);
#endif

static void do_debug_key(unsigned char key, struct cpu_user_regs *regs)
{
    printk("'%c' pressed -> trapping into debugger\n", key);
    (void)debugger_trap_fatal(0xf001, regs);
    nop(); /* Prevent the compiler doing tail call
                             optimisation, as that confuses xendbg a
                             bit. */
}

void __init initialize_keytable(void)
{
    register_irq_keyhandler(
        'd', dump_registers, "dump registers");
    register_keyhandler(
        'h', show_handlers, "show this message");
    register_keyhandler(
        'q', dump_domains, "dump domain (and guest debug) info");
    register_keyhandler(
        'r', dump_runq,      "dump run queues");
    register_irq_keyhandler(
        'R', halt_machine,   "reboot machine");

    register_keyhandler(
        't', read_clocks, "display multi-cpu clock info");

#ifdef PERF_COUNTERS
    register_keyhandler(
        'p', perfc_printall, "print performance counters");
    register_keyhandler(
        'P', perfc_reset,    "reset performance counters");
#endif

    register_keyhandler(
        '0', dump_dom0_registers, "dump Dom0 registers");

    register_irq_keyhandler('%', do_debug_key,   "Trap to xendbg");
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
