# Where a change goes

Each decision the compiler makes about a program has one home: a layer that
decides it once, in the analysis and representation layer, and that the
emitters read (#7100, #8328). A fix that does not know those homes tends to
land as a special case at one call site: a name test in one emitter, a fact
re-derived where it is needed, a layout read off a type. It answers the
program that found the bug and leaves the next one wrong, and review sends
it back. This page lists the layers, the question each one answers, its
entry points, and how the old way looks in a diff.

Three rules follow from it:

1. **Put a fact in the layer that owns it.** Find the layer in the table
   below before writing code, and make the fix there.
2. **Convert old-style code on the lines you touch.** A line the change
   edits anyway asks the layer, not the old test beside it.
3. **When the right layer lacks the fact, add it there.** Extend the row,
   the plan or the predicate rather than special-casing one call site. A
   fact that needs new analysis is its own change, made first.

## The layers

| A change answers...                                                                                               | Layer                                              | Entry points                                                                                                                                                | The old way, in a diff                                                                                                                                                                                                                                                                                                 |
| ----------------------------------------------------------------------------------------------------------------- | -------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| what a builtin call answers, its arity, its block form, how it is emitted                                         | builtin-op rows                                    | `builtin_ops.c:bop_find`, `bop_find_arg`, `bop_result_args`; inference `analyze_infer.c:an_bop_find`, `an_bop_result`; emission `codegen_ops.c`             | an `sp_streq(name, ...)` arm in inference or an emitter that a row could state; inference computing a result beside the row it found                                                                                                                                                                                   |
| what a builtin does with the Strings it is handed                                                                 | builtin-op share rows                              | `builtin_ops.c:bop_share` (`BSH_*`)                                                                                                                         | a name test for a builtin's keep-or-answer contract inside a walk                                                                                                                                                                                                                                                      |
| which user method a call reaches, its refusal, its poly dispatch, its nil test                                    | the call plan                                      | `call_plan.c:cplan_user`, `cplan_user_in`, `cplan_refuse`, `cplan_poly_arms`, `cplan_targets`, `cplan_nil`; `codegen_util.c:refuse_from_plan`               | a read of the method tables (`comp_method_in_chain`, `comp_reader_in_chain` and the like) outside `call_target` in a pass or an emitter; method-chain resolution or rebuilt dispatch arms in an emitter; a refusal keyed on a name alone; a nil test written by an emitter                                             |
| where a definition, constant write, class body or require stands in load order, and whether it has run at a point | the load record                                    | `analyze_elab.c:elab_point_of`, `elab_order`, `elab_const_at`, `elab_written_before`, `elab_method_at`                                                      | an order read from node ids; a walk over every write of a name to decide whether one ran first; a write in a branch taken as sure; `ELAB_UNKNOWN` read as no                                                                                                                                                           |
| which version of a method a call reaches where it runs, and how a pass that appends or edits nodes asks it        | `call_target` and the call plan's frame            | `call_plan.c:call_target`; `analyze_elab.c:elab_plan_enter`, `elab_plan_leave`                                                                              | the record's map asked for a call's version per node by a pass that edits between its asks; a method found by name where `call_target` binds the version; a method-table read (`comp_method_in_chain`, `comp_reader_in_chain` and the like) in a pass or an emitter, where `call_target` or `call_attr_target` answers |
| which class side owns a def, attr, visibility, alias, undef or mixin                                              | the registrations from the load record             | `analyze_elab.c:elab_entries`, `elab_class_surface`, `elab_class_mixins`, `elab_class_bodies`, `elab_defines`, `elab_def_applied`                           | a walk over class bodies' statements for a definition form; a shape (a wrapper, a condition's arm) taught to that walk rather than to the record                                                                                                                                                                       |
| what a module method's, or a class's, `super` reaches: the owner after a def at each position                     | the load record                                    | `analyze_elab.c:elab_module_super_owners`, `elab_class_super_owner`; the builtin side `builtin_names.c:builtin_ancestor_at`, `builtin_owns_instance_method` | a scan of every node per forwarder; a name list standing for "a builtin may answer"; a second reading of the class bodies for their mixins and defs                                                                                                                                                                    |
| which classes a value can hold                                                                                    | the class facts                                    | `analyze_cls.c:cls_fact_node`; `compiler.c:comp_poly_candidates_at`                                                                                         | every class that defines the method's name taken as a target                                                                                                                                                                                                                                                           |
| which builtin classes a value can hold, and which of them a boxed send tests                                      | the class facts' builtin part                      | `analyze_cls.c:cls_fact_may_hold_builtin`; `call_plan.c:cplan_send_split_owners`                                                                            | every builtin class that answers the name taken as an owner                                                                                                                                                                                                                                                            |
| whether a scope or protected body may raise or non-locally unwind, and whether a `begin` or rescue modifier elides `setjmp` | the raise facts                                    | `analyze_raise.c:an_raise_facts`, `an_scope_may_raise`, `an_expr_may_raise`, `an_begin_elides_setjmp`, `an_rescue_mod_elides_setjmp`, `an_rescue_elided`    | a `begin` emitter and `is_setjmp_construct` deciding separately whether a handler emits `setjmp`                                                                                                                                                                                                                       |
| a String handle from a call or held operand | `Repr` and operand views | `repr_native_call_handle`, `emit_strbuf_handle_of` | reading bytes before a later operand runs |
| layout in codegen: boxed, scalar, sentinel, handle, struct                                                        | `Repr` and the type traits                         | `repr.c:repr_of`, `repr_of_slot`; `holder.c:holder_of_node`; `types.h:ty_traits_of`; `codegen_util.c:emit_coerce`                                           | layout decided from `comp_ntype`; hand-written boxing or nil tests; a cast into a slot instead of `emit_coerce`                                                                                                                                                                                                        |
| where a value comes from, how its parent uses it, a Prism flag                                                    | the value-position walkers and the node predicates | `value_pos.c:for_each_value_position`, `vp_one`, `vp_climb`; `node_table.h:nt_paren_single`, `nt_safe_nav`                                                  | recursion over `elsif` or `rescue` with a depth limit; `sp_streq(op, "&.")`; a raw flag bit                                                                                                                                                                                                                            |
| a fact about one emission, or one poly arm's state                                                                | views and `ArmCtx`                                 | `codegen_view.c:view_push`, `view_push_repr`, `view_bind`, `view_push_arm`                                                                                  | a node's type, flag or name overwritten and restored around an emitter call; a new `g_*` global                                                                                                                                                                                                                        |
| membership in a family of names                                                                                   | the name predicates                                | `builtin_names.c:is_call_alias`, `is_then_alias`, ...                                                                                                       | the same inline `sp_streq` chain in inference and in codegen                                                                                                                                                                                                                                                           |
| when a pass runs, and what inference reads                                                                        | the phase driver and the counted setters           | `analyze.c:analyze_program`; `comp_set_ntype`, `lv_set_type`, `ivar_set_type`                                                                               | a loop appended to `analyze_program`; a direct write to a table `infer_type` reads                                                                                                                                                                                                                                     |

A boxed Proc call preserves the distinction between a positional trailing
Hash and explicit keywords. An empty keyword splat still affects whether
the Proc unpacks a positional Array.

String slots hold object handles. Sharing facts and byte routes serve
builtin producers still being migrated under #11391.
Boxing a String preserves the object emitted by its producer. A dynamic
send uses that same path for the String returned by its selected method.
The selected, blockless unary plus on a core String reads and returns a
handle. A mutable String keeps its identity, and a frozen String yields
a mutable copy. The block and String subclass call paths retain their
own result handling.
The safe-navigation guard holds a nullable String result through the
call. It evaluates arguments in the nonnil arm and uses the same handle
form for inline and hoisted results.
Hash transformation uses the block's ordinary result when storing
a boxed value, including a result returned through `next`.
A nullable Integer or Float result keeps its nil through boxing.
For a typed Integer destination, the transform writes the value and its
nil flag together. Both come from the receiver when changing the keys
and from the block when changing the values.
A builtin key transform uses a general Hash when no typed layout holds
both the new key and the original value.
Hash key insertion consults the runtime String's frozen state. It retains a
frozen key and makes a frozen key object for an unfrozen input, including a
static empty or one-character result. The Hash stores the key's byte view.
An ordered typed store holds a String key through evaluation of the value
and takes the key's bytes when inserting the pair. A boxed key stays boxed
until the value has run and the store has checked whether the Hash is frozen.

Conditional method dispatchers expose real call arguments even when their
parameter names are synthesized. The shared argument binder judges those
counts for ordinary calls and implicit conversion callbacks alike. A
possibly absent dispatcher uses its existing availability selector to guard
count errors, after evaluating the arguments; its missing-method arm still
owns the absent case. Variable-arity dispatchers forward their arguments to
the selected arm, where that method's defaults and keywords are checked.

A default argument is evaluated on the callee. Binding a String default
uses the same handle as the expression that supplied it.
An ordered operand keeps its saved handle until its argument binding is
removed. For a call with a native String result, it holds the handle
from the call's result. An abandoned emission also removes the binding.
Safe navigation holds the call's native String result through its nil
test and argument evaluation. The selected call can read the held
String's current bytes after an argument mutates it.
Boxed callable dispatch holds the receiver and arguments before it
binds the block operand. For safe navigation, the argument statements
and explicit block expression run only after the held receiver passes
the nil guard. Proc invocation carries the captured block and
the keyword marker through the same call as the argument vector. A boxed
curried Proc forwards the block of the call that completes its arguments.
An Enumerable fallback in boxed dispatch uses the same staged block as
the builtin and user arms, with the receiver held across its conversion.
A composed Proc forwards the block to its first function and passes that
function's result to the second.
A call that materializes a curried block uses its runtime Proc wrapper.
The wrapper retains the Curry and receives the arguments and block of
each later call. Boxed dispatch passes the staged Proc to the selected
method.

Lazy `drop_while` binds its predicate with the same argument layout as
the other fused stages. Destructuring and optional, rest and numbered
parameters are handled before testing the predicate's result.

A scoped String local stores an `sp_String *`. A byte reader projects
the payload from that object at its use. Nil uses a null handle.
Temporary emission bindings use the type of the C declaration they name.
A direct call holds its receiver when the operand-order record marks a
read that a later operand can rebind. The temporary uses Repr's layout
and retains references while the arguments run.

The operand-order record lists an indexed operator write's receiver,
keys and right-hand side in Ruby order. The native indexed writer takes
the receiver before emitting an AHEAD key's statements. Both statement
and value forms consult the receiver's HOLD slot and its holder's Repr
when deciding whether its temporary needs a GC root. The getter runs
before the right-hand side.

A method inferred or declared to return String returns `sp_String *`.
The String addition emitter boxes its result when Repr selects a boxed
result for the call, even when both operands are known Strings.
`repr_method_ret_storage` supplies the physical result type to signatures,
return slots and callers. A nullable String uses the same pointer and returns
`NULL` for nil. The caller can retain the handle, box it for a Ruby value,
or read its bytes for a text operation. An initializer used for its effects
has a void C signature even if its last expression is a String. Other String
methods return their selected object even when the caller drops it. A nil arm
joined to a String handle uses a null of that pointer type, so boxing the
result still selects the handle representation. Mixed receiver dispatch joins
Ruby values in the switch and projects the result once. Proc and C extension
callback signatures retain their declared result representation.
A builtin row can declare the same native handle result with `BuiltinOp.result`
set to `TY_STRBUF`. Semantic inference reads the result as `TY_STRING`.
The call plan gives the renderer and Repr the same selected producer. Boxed
dispatch arms use the row they emitted to box the result. An override-capable
display conversion still returns its actual boxed value.
String `tap` retains its receiver across the block. The native result view
returns that handle and a byte consumer reads its payload afterward.
A boxed String carries the `sp_String *` handle in an `SP_TAG_OBJ` box
with class ID `SP_BUILTIN_STRBUF`. This includes frozen literals and the
objects retained by Hashes, Enumerators, and callbacks. Ordinary boxing
reads the expression's representation, including results held by a call
or written into a slot. A boxed inline yield keeps the block's String
handle, including a value supplied by an early `next`.
A rescue modifier with a demanded String result takes each arm into the
same handle slot, whether the arm creates a String or returns an existing one.
The destination's boxed representation takes precedence over a native
String source for both `next` and normal block completion. Byte comparisons
and private conversion buffers can borrow its payload through
`sp_rbval_string_bytes`.
An inline Array or Hash block result supplies its declared kind to empty
`next` literals. Its result slot stays rooted while `ensure` runs before
the caller takes the value.
Boxed Array pushes hold each argument across evaluation of the next.
An Array constructor's block result uses its element storage representation.
A String result stays in a rooted handle slot through `ensure`, then the
Array takes the same handle whether the block ends normally or uses `next`.
Array element stores hold the receiver, index, and value before a checked
index conversion. The frozen check precedes that conversion.
The capitalization routines return an empty operand's byte view. Boxing
that result preserves the original empty String and its encoding.
The ENV read paths that return a String default or a missing-key block's
String use the same result view. The selected branch hands back its
String handle directly.
IO reads hold the output buffer through argument evaluation. The runtime
converts it once and writes through the resulting String handle. A
successful read returns that handle. Nonblocking reads with a possible
wait Symbol keep their boxed result. A zero-length read checks the stream's
readability and returns before waiting for input.
The native String result passes its handle directly through C. Byte
consumers project the contents and take a snapshot where the read is
held across a block. UTF-8 character positions count each valid sequence
once and each malformed byte separately. The length cache records only
validated text, so validation and optimized character seeks can reuse it.
A named reflective write to an existing String instance variable uses
the ordinary String slot writer. It holds the original receiver before
running the assignment's right-hand side and checks whether that receiver
is frozen before storing the value.
Exception construction keeps the message's String object and projects
bytes where the constructor reads them. A typed raise passes that handle
to the runtime. Cleanup keeps the cause of an exception it will rethrow.
Multiple assignment holds each
String through later values and writes. A value-producing assignment
returns those same objects. File.join keeps the bytes of each boxed path
component through later conversions.
Native package readers that declare a handle result return it through the
same call view, including inherited bindings and calls through super.
A native binding uses the producer's declared String
result, the handle of a held operand or slot, or a checked projection from
the boxed Ruby value. The read of an existing String keeps its object.
When Repr says a builtin result needs a handle formed from its bytes,
the String slot renderer wraps the result where the slot takes it.
Boxing a reflective String read uses the handle from the selected slot.
An ENV write holds both Ruby operands, converts the key and then the
value, and reads their bytes for the environment after the conversions.
The setter method returns the converted String object. The assignment
expression retains its original value operand.
Fiber context switches carry the active GC roots and exception state.
Each protected region on a `redo` transfer runs its `ensure` before
control returns to the loop or block body. The transfer leaves the
exception and rescue frames of the regions it crosses.
An ensure suspends a pending nonlocal transfer while its body runs.
Normal completion resumes the saved transfer. A transfer leaving the
ensure body replaces it.
Each jump to a protected region drops the handler scopes it crosses.
The remaining targets stay available while the ensure runs.
A catch remembers the active Proc return homes at entry. A throw
restores that chain when it lands, retiring the homes of exited methods
while keeping homes in the enclosing calls.
An untyped begin in value position uses a boxed result slot across rescue
and ensure. When the enclosing slot holds an optional number, it receives
the boxed result through the same nil-preserving conversion as a return.
A tail break transfers control to its statement owner before
the enclosing expression can store a fall-through value.
A receiver-answering iterator at method tail keeps its receiver in the
operand view across the block and returns it through the physical result
slot, including a String handle slot. Builtin rows identify self answers;
the subclass adapter retains the original object when its builtin reads
String bytes. A block break keeps its own boxed result outside that adapter.
A keyword loop in value position supplies its own result slot. A nested
statement loop evaluates its break value without writing the enclosing slot.
A for loop evaluates its collection before entering its own control context.
A break with several values supplies one Array to its target. The jump
lowering includes a trailing keyword hash among those values.
The ordinary expression view also keeps the handle for a builtin
receiver-answering String iterator.
Native String freezing and frozen-state checks use the ordinary operand
handle, including a String returned from a call or read from a container.
String#append_as_bytes holds the evaluated operands as Ruby values. It
validates their types, grows the receiver once and publishes the new length
after copying, so each self argument contributes the original bytes.
String line methods retain the receiver through separator and chomp
evaluation. With a nil separator the receiver is the only line, including
for an empty String. A line Enumerator retains its receiver and separator
and reads their contents when each walk begins.
A foreign binding that loans bytes copies them into a String handle before
an allocation can retire the provider's buffer. Binary bindings pass their
published count so embedded NUL bytes survive.
Interpolation and printing call the display method selected at that site.
They read the bytes of its native String result or format its boxed result
while the receiver is rooted. Interpolation keeps a mutable conversion
result until its literal segment is assembled, then reads its bytes and
length together. Adjacent segments finish in order. A segment's argument
evaluations and encoding checks run before the next segment starts.
This includes registered class conversions and a rescued
exception’s override or stored message. A generated String member reader named to_s or inspect
returns its stored field. The default Struct or Data renderer produces
counted text. A segment can write a final builtin repeat into its counted
destination after evaluating its String and Integer operands.
String comparisons borrow bytes from the selected conversion
result. Case comparisons hold the receiver through `to_str` and read its
current bytes after conversion. Boxed String readers use the load record
to select their builtin or an installed override. An alias that captures
the builtin keeps that implementation in a method scope. A prepend's guard
and its `super` share the version history of the String method behind it.
A placed conditional prepend activates its guard when its statement runs.
Dispatch and `respond_to?` use the active arm's visibility and fall back
to the underlying method while the guard is clear.

MatchData's named capture hashes and boxed String scan results construct
String handles from the counted capture spans. A participating empty
capture has its own mutable String. A missing capture occupies a nil slot.

A String Range stores the two endpoint handles. The same layout serves
typed and boxed membership, coverage and Marshal operations. The typed
builtin readers publish their result through the ordinary String handle
ABI. An operand held for a later call keeps the returned handle.
A reader that needs bytes borrows them from that handle. The boxed
`to_h` conversion checks the iteration, which can yield a member when
`min` is nil.

A conditional assignment to a String instance variable evaluates
the right-hand side only when the store is taken. It checks whether the
receiver is frozen after evaluating that value and before updating the slot.

A String subclass embeds the String handle as its leading payload. Repr
identifies this layout when a subclass passes self to a reopened String
method; the caller passes the existing handle, preserving identity and
mutation through the superclass call.

An immediate String append can consume the bytes of a boxed builtin
`to_s` directly. The call plan allows that view when the recorded call
selects the builtin display path. Calls that may reach another method
produce the usual result. A multi-argument append stages its argument
values before the first write.

Process spawning and the JSON, StringIO and PTY packages read boxed base
Strings through
`sp_rbval_is_string` and `sp_rbval_string_bytes`. The byte accessor borrows
the current contents and leaves the original box intact. Keep that box
or its owner live across operations that can allocate.

StringIO retains its backing String. A read into a supplied buffer
retains the converted destination through length conversion and
publishes its bytes and encoding on that object.

The load record's rows are described in [load-model.md](load-model.md):
what the record holds, what its answers mean, and what keeps it cheap for
a pass that edits the tree.

Codegen reads decisions, not types. A `comp_ntype` read in an emitter
answers one of two questions, and both have a layer:

- how the value is laid out (boxed, nullable, a handle): `repr_of`;
- what to do with it (this receiver is an Array, so the Array emitter;
  this call reaches that method; this arm is that class): the call plan
  and the builtin-op rows, which hand the emitter the kind, the row and
  the target to switch on.

Reads of the second kind are many today and go as the plan and the rows
take over each emitter. A read stays only where the type is itself the
data, such as a class name in a diagnostic.

Native integer Range predicates hold the receiver as a value and use the
runtime comparisons that also serve boxed Ranges. Constructing the boxed
argument cannot collect the receiver.

## Views

The analysis decides one answer per node: its type, its representation flags,
which method a call reaches. Codegen often emits the same node more than once
under different assumptions: inside each arm of a poly dispatch, where the
receiver is one class; as a temp a receiver was materialized into; with a
flag lifted for one re-entry; as the text of a hoisted argument.

A view says that for one nested emission only. `view_push` (a type),
`view_push_repr` (a representation flag), `view_bind` (the text written for
a node) and `view_push_arm` (a dispatch arm's context) record the previous
answer and install the new one; `view_pop` restores it. Everything that
reads the node in between (`comp_ntype`, `repr_of`, `emit_expr`) sees the
view, and nothing outside it does.

They are one stack rather than ad hoc overrides because each of these went
wrong by hand:

- **An early exit leaves the override behind.** A refusal unwinds every
  view, binding and arm context opened since its `view_mark()`
  (`view_unwind`).
- **Nested overrides restore out of order.** A pop out of order is an
  internal error, not a silent wrong answer.
- **A cache keeps a temporary answer.** Anything that memoizes per node
  caches only with no view open, or keys on `view_epoch()`.
- **An override is invisible.** Every one is on the stack, and `repr_of`
  stays a pure read of it, which `--repr-check` relies on.

So codegen changes how a node reads for one emission only through a view,
and never writes the node table or the analysis's answers itself. A fact a
view cannot yet carry gets a new kind of view.

## Reading a diff for the old way

A change that does any of these is asking a question one of the layers
already answers, or should:

- a new `comp_ntype` read in codegen, whether it decides layout or which
  emitter, builtin or target runs (a read whose type is the data, such as
  a diagnostic, is fine; say so);
- a call-plan or row change that records a decision for `--plan-check`
  while the emitter beside it still decides on its own;
- an inline `sp_streq(name, "...")` where a row or a predicate fits;
- a renderer that reconstructs a String result from its source slot after a callback;
- a special case keyed on one method name;
- hand recursion over node kinds where a walker exists;
- a fact re-derived at one call site that a layer already answers;
- an order of two nodes read from their ids, or a walk over every write of
  a name to decide whether one ran before a point, where the load record
  answers (`elab_order`, `elab_const_at`, `elab_written_before`);
- a pass that appends or edits nodes and asks the load record between its
  edits: each ask after an edit rebuilds the record's map, which makes the
  pass quadratic in the program (#10422, #10490);
- a list of names, or a walk over class bodies, collected for a fact the
  load record holds (`elab_names_method`, `elab_defines`, `elab_entries`).

## The verifiers

Each layer has a check that compares what it decided with what the
emitted C does. A change to a layer keeps its check at zero, or lowers
the count it reports.

| Verifier               | What it compares                                                                                                                                                                                                                                                                   | How it runs                                                                                                                                                            |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `--plan-check`         | the builtin-op row, the user method and the dispatch arms codegen emitted a call through, against the ones inference answered it with                                                                                                                                              | `tools/plan_check.sh` over the corpus; a conflict fails                                                                                                                |
| `--method-table-check` | each read of the method tables made outside `call_target`, by family (call, implied, presence, super, send, lists, other, pass), and each answer of `call_target` that is the table's last definition for want of a proof (`table <why>`)                                          | `make method-table-check`, `tools/method_table_check.sh` over the corpus; a read in any family, a program that fails only under the flag, or a bracket left open fails |
| `--repr-check`         | codegen asks `repr_of` of every node it emits; the C must not change                                                                                                                                                                                                               | `make repr-check-test`                                                                                                                                                 |
| `--check-traits`       | every column of the type traits table, rendered from the function it summarizes                                                                                                                                                                                                    | `make traits-check-test`                                                                                                                                               |
| `--check-stores`       | each value written raw into a C slot of another C type                                                                                                                                                                                                                             | `make check-stores-test`                                                                                                                                               |
| value edges            | the generated table of how each node kind's parent uses its value, against its source                                                                                                                                                                                              | `make value-edges-check`                                                                                                                                               |
| counted setters        | the tables `infer_type` reads are written only through their setters                                                                                                                                                                                                               | `make ty-setter-check`                                                                                                                                                 |
| `SPINEL_ELAB_CHECK=1`  | the load record's order against `prog_order` and `prog_sort`, the map and the record the walk made against ones made after it, class headers, def owners, the class tables against the record's surface, and each call's version in the call plan's frame against the map as it is | set on any compile: `spinel-elab-check:` lines on stderr, each family's counts last                                                                                    |
| load-graph test        | each test/load_graph program's output under `--load-model=graph`, and its `--dump-load-record` against its `.load-record`                                                                                                                                                          | `make load-graph-test` (part of `make test`)                                                                                                                           |
| load-record oracle     | the load record's definitions, constant writes, class bodies and mixins against those CRuby's load phase runs, traced                                                                                                                                                              | `ruby tools/load_record_oracle.rb PROGRAM...`                                                                                                                          |
| incremental-test       | an incremental build's binary against a from-scratch build's, and the misses of a local edit against a bound                                                                                                                                                                       | `tools/incremental_test.sh` (`make incremental-test`)                                                                                                                  |

The checks report; they do not decide. When a check and the C disagree
and the C is right, the layer's answer is what to fix, so the two agree.
