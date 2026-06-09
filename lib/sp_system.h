/* sp_system.h -- system()/backtick support in libspinel_rt.a.
 *
 * `sp_last_status` backs Ruby's `$?` (the wait status of the last
 * `system` / backtick). `sp_system_args` runs argv[0] with the rest as
 * arguments (the multi-arg `system(...)` form), setting `sp_last_status`
 * and returning whether the child exited 0. The bool result is declared
 * as `int` to stay decoupled from the runtime's mrb_bool typedef and the
 * mruby_shim.h conflict -- same convention as sp_core.h / sp_time.h.
 */
#ifndef SP_SYSTEM_H
#define SP_SYSTEM_H

extern int sp_last_status;
int sp_system_args(int argc, const char *const *argv);

#endif
