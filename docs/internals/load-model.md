# The load model

CRuby builds a program's classes by running it: the files in the order
they load, each file's statements in order, each class body where it
stands. Whether a method was defined before a call runs, which class a
constant holds at a reference, which class side a `def` lands on: each is
a question of that order. The load record (`src/analyze_elab.c`, #9898)
answers them. Node ids do not: they are one file's order, and a
desugaring appends nodes past every file. A walk over every node that
writes a name does not either: it cannot tell a write that ran from one
in a branch that did not, or one that runs later.

This page says what the record holds, when it is made, which queries read
it, what keeps it cheap, and how it is checked.
[architecture.md](architecture.md) lists it among the layers.

## Points and statements

The record follows the load phase over statements (`elab_points_walk`,
from each file's root). Every statement it reaches is an `LrStmt` with a
point, a `LoadPoint` of (region, seq): seq counts the statements in the
order they run. Region 0 is the load phase the load graph places; a file
it does not place is a region of its own (see "Requires and the two
models"). Every other node takes the point of its nearest walked
statement, found up its parents. `elab_point_of` answers a node's point.

At each step `elab_child` decides where the load phase stands. The walk:

- enters a file at its load: its root, its `BEGIN` blocks, then its
  statements;
- enters the bodies that run where they stand: class, module and
  `class << x` bodies, `ensure` clauses, and the blocks the load phase
  runs at once (the eval and exec forms, the class
  builders such as `Struct.new` and `Class.new`, the builtin iterators);
- enters the statements of a branch, a loop's body, a rescue clause, the
  right side of `&&` and `||`, and the value of `||=` and `&&=`, which
  may not run: they are `may`, and a loop's are also `rep` (below). A condition whose predicate is a constant
  (`nt_pred_const`) is folded: its arm runs whenever the condition is
  reached, and the arms after one that always runs never do;
- enters a block given to any other call, a program method's, as `may`:
  the method may yield to it before it answers, or keep it. A block that
  never runs has no statements to enter: where a call on self in a class
  body, at a point that runs once, hands one to a class method of the
  program's and no method of that name uses its block
  (`elab_block_never_runs`), the scope walk leaves the block's body out;
- does not enter method bodies, lambdas, and the blocks kept to run later
  (a Proc, `define_method`, a thread, `at_exit`:
  `is_deferred_block_call`). Their nodes have no point: `elab_point_of`
  answers region -1, and `elab_point_from` the point of the statement
  that defines or makes them.

An `LrStmt` carries, besides its node and point, how it runs:

- `may`: it runs only under a condition;
- `partial`: it runs at once if at all, but maybe not at all or cut
  short, in an iterator's block (zero times, or several) or a `begin`
  body with rescue clauses (a raise skips the rest);
- `rep`: the nearest statement around it whose body may run more than
  once (a loop, an iterator's block, a `begin` a rescue may `retry`), so
  a statement after a point may also run before it;
- `last`: the last statement of the bodies it runs, which complete before
  it does (`elab_point_done`);
- `frame`: the statement whose body opened its class frame, -1 for a
  file's top level.

A class, module or `class << x` body, and the block of an eval form or a
class builder, opens a class frame: `lr_frame` decides its class and
whether it is that class's singleton side, main's at a file's top level.
`fmay` and `fpartial` are `may` and `partial` counted from the frame
rather than from the file. A class body opened in an iterator's block, or
in a `begin` with a rescue clause, is partial as a whole, but whenever it
runs its own statements run whole, so their definitions apply to the
class (test/class_body_in_block_runs_whole.rb).

`--dump-load-record` prints the record and stops the compile. For
test/load_graph/record_main.rb, part of the points reads:

```
r0 #9 f0 ClassNode Shape line 9
  r0 #10 f0 CallNode attr_reader line 10
  r0 #11 f0 CallNode each line 11
    r0 #12 f0 CallNode define_method line 11
  r0 #13 f0 IfNode line 12
    r0 #14 may f0 DefNode odd#__cond1 line 13
```

(`r` the region, `#` the seq, `f` the file), and the histories follow the
points (record_main.rb.load-record).

## Histories

`elab_program` makes the histories. It runs the scope walk of each file's
top level in the order the load phase runs them, and records each
statement once the walk has passed it, in seq order, in its frame
(`lr_record_stmt`):

- a method's entries by (class, singleton side, name), main's under -1
  (`LrDef`, of kind `LrKind`): `def`, `define_method`, the attr forms,
  `alias` and `alias_method`, a `module_function` copy, `undef` and
  `undef_method`, `remove_method`, and visibility changes;
- a class's bodies (`LrOpen`), mixins (`LrMixin`) and superclass;
- the writes of globals, main's instance variables and constants, each
  constant under the class whose body is its lexical scope;
- what a class's body ran that the record cannot follow (a `send`, an
  eval of a String, a mixin of a module it does not know:
  `elab_opaque_before`), and whether a definition form runs code of the
  program's, a method of its name or a hook (`elab_form_runs`).

An entry keeps its node and reads its point from the map, so a rebuilt
map moves none. An entry recorded under a condition is `may`: it may not
have run. A write is `may` also when its statement is `partial`, and so
is every `||=`.

The histories hold what the load phase runs. A definition or write in a
method body, a lambda or a block kept to run later has no point, so no
query at a point sees it. Most are in no history; three kinds of entry are:
a public keyword `def` whose class the record resolves, a version with the
body's extent (`LrDef.body`, `lr_version_order`, "Versions bodies make"
below); a constant write marked `LR_LATE`, held as a possible write
(`lr_record_unplaced`); and a visibility word that runs later or is sent,
held on the table it names (`lr_unplaced_words`). A class body that is no
statement of the load phase (in a lambda, in a block kept to run later, a
class builder's block in an expression) is its class's body all the same:
its public defs are versions of the body around it, and its other
definitions are recorded after the placed bodies, `unplaced`, so the
registrations apply them but no query at a point sees them
(`lr_record_unplaced`, `lr_body_defs`).

Some answers wait for the whole record. A file in a region after region 0
is walked after everything the entry does, so the answers recorded on the
way (a mixin's module, a form's "runs program code", an alias's
visibility) are asked again once all of it is made (`lr_settle_regions`).
A class builder's class (`X = Struct.new(:a) do ... end`, a local's too) is
made where the walk reaches its write (`record_write_class`), so its block
is recorded as that class's body like a header's.

### Extents

Code that runs when something else does has no point; its **extent**
stands in for one (`LrExt`, `lr_extents`, one entry per body, built after
`lr_run_points` from the record alone: syntax and `lr_rta`'s own call
binding, never types). A body is a method body, a lambda or proc, a block,
an `END` or `at_exit` body, or the file root of a region no load places.
The record holds, for each:

- `from`: the first statement of region 0 it may start on (`any` when the
  record has no fact, `never` when no run reaches it, or its count is
  `zero`). `LR_POST` (`post`)
  is the phase after the load: it comes after every statement of region 0,
  and is where exit handlers start (an `at_exit` call only where no method
  of the program's may take the name, `lr_named`, as one may run the block
  at once) and where `lr_rta` seeds the
  `--ext-entry` methods (see "The post-load phase" below). A region's `from` is the statement its load may
  first run on (`rentry`); a node of a body that has no point of its own
  starts no earlier than the body around it. A lambda or proc starts at the
  first statement one of the calls that invoke it may run on, where the
  record knows them all (`lr_closure_sites`): its value goes to a local
  whose reads are each the receiver of `call`, `()`, `[]`, `yield` or
  `===` (a name no method of the program's takes), or to a constant whose
  reads, by name anywhere in the program, all are. That holds only in a
  program that reaches no constant or local by a name it computes
  (`lr_const_reached`): no `const_get`, `remove_const`, `constants`,
  `binding`, `eval`, `each_object` or kin called directly, through `send`, `__send__` or
  `public_send`, as a Method or as an `&:name` block; no `class_eval` or
  `instance_eval` of a String; no Method made of, and no `alias` or
  `alias_method` taken of, any of those, a send or a Method maker; no
  send, Method or `alias_method` of a name that is not a literal; no load
  of code the record has not read (`elab_runs_unseen`). The
  block of a `lambda` or `proc` call is such a closure only when no
  method of the program's may take that name (one it defines, or one a
  computed name may make), since that method may run the block at once.
  With no invoking call it never runs; past any other use it starts at
  the statement that makes it.
  A `define_method` or `define_singleton_method` block of a version the
  record holds starts as a method body does, at the first statement a call
  of the method it makes is reached on (`lr_rta`), and never where none is.
- `sure`: the first statement after which it has certainly run at least
  once, or `never`. A method body is sure at a call that binds it alone
  (a constant receiver the record resolves, one public def of the name on
  the chain, made before the call) when the call runs whenever its
  statement does (`lr_runs_with_stmt`: every edge up to the statement is an
  assignment's value, a call's argument or receiver, parentheses or a
  statement list's member, none a block's or a `&.` call's argument, so no
  `&&`, ternary, modifier `if`, `case` arm, `defined?`, op-write or `begin`
  around it inside the statement) and that statement is not conditional
  (`may`: also a class body under a condition), in a block that may not run
  or under a rescue. An in-place block on a literal
  is sure at its call under the same tests, when its row runs it for
  certain (`IR_EACH` on a literal of some elements, `times` of one or
  more, `tap`); `0.times` and an iterator that may skip its block, such as
  `uniq` or `slice_before`, are never sure.
- `count`: the most runs after `from`, `zero`, `once` or `many`. A method
  called by one call that runs once, which nothing else of the program can
  reach, is `once`; a block of a literal receiver counts by its iterator
  row (`IterRow.runs`: once, by the receiver's value, a step per element;
  a call with no receiver reads Kernel's, `catch`), and a block, lambda or
  exit handler with a `redo` in it is `many`. A lambda or proc whose invoking calls are known
  is `once` when one call invokes it, that call runs once and its body has
  no `redo` (its
  statement does not repeat, no loop stands between them, and the body
  around it, if any, runs once).
- `kept`: it runs at a time the syntax does not tell (a block whose call
  keeps it or the record does not know, a lambda, a handler).

#### The post-load phase

The exit handlers (`at_exit` blocks, `END`) run after the last statement of
the load. `lr_rta` does not scan a handler's body where the handler is
registered: its nodes are a unit of their own (`lr_handler_body`), queued
where the registering call is scanned (`lr_rta_handler`, also from a
method the load reaches, or from another handler), and scanned once the
load statements are done, in a post pass with the `--ext-entry` methods.
That pass reaches again what the handlers call, from the end of the load:
a method no load statement reached starts at `LR_POST`, and one the load
reached too keeps its `from`. A handler registered by a handler, or by an
`--ext-entry` method, is queued as it is scanned. A method that a handler registered
during the load calls does not run from the registering call.

The pass does not order the handlers against each other (CRuby runs them
last-registered first, an `END` on the same stack, a handler registered by
a handler after the one that registered it, and the statements of one
handler in order).

Whether code may run after the load is one fact of the record
(`LoadRecord.post`, set in `lr_rta`): the program has an exit handler, an
`at_exit` call or an `END` anywhere, reached or not, or is built with
`--ext-entry`. The record does not follow every path to a body a handler
runs (a closure kept in a global, an ivar or a table, a handler given by
`&`, a Method's `call`, a method a closure calls), so it cannot prove that
any body is done with the load, and where `post` holds every call in a
body, a block or a lambda has `ElabWindow.post`. A call in a load statement
does not, unless it stands in a block the record keeps to run at a time the
syntax does not tell (`LrExt.kept`) and gives a point of its own (a Hash's
default block, a lazy chain's block: the dump shows a point for them, though
the block may run after any later statement and from a handler; an
Enumerator's, a `proc` and a `define_method` body have none and are
answered by their window). That holds with no handler too: it is
`elab_in_record_kept_block`, in any program, and the call plan asks such a
call's versions as it does for a block that runs later (`ct_meet_how`,
`ct_version_at`) instead of answering from its point alone, so a def made
after the block and before it runs is met. Only a block a builtin may keep
counts (`LrExt.kept` with a call answer of "later" or "not known"): the block
of a call the record takes to run it at once by its name or its receiver (an
iterator over a local, `tap`, `then`: `ELAB_BLK_ASSUMED`, a lazy chain's
name among them) and one given to a method of the program's (`try { }`, which
may yield at once as well as keep it) keep the answer of their point, as does a
block the record proves runs at once. Even a kept block keeps its point's
answer where its window holds a def of the top-level statement the call stands
in, or cannot tell (`ELAB_MEET_RECORD` in `elab_call_meet`): a class body
applies its defs as the statement starts, so a cell would already hold the last
of them, and a block that runs during the statement (a Hash default block
called there) meets the version at its point. (Only
the answers that cannot change during the load, `name`, `to_s` and `inspect`
(`is_name_reader`), are given a class body's `self` as their receiver in a
block of its body. `ancestors`, `include?`, `is_a?` after an `extend`, and the
method-table readers (`instance_methods`, `method_defined?`, `instance_method`)
answer from the point the block runs at, which the plan does not place yet for
a def or an include made later, so a bare one in a block stays refused.) A call with
`ElabWindow.post` has a window that holds the versions bodies make that
first run after the load, so a
version a handler makes is one more version in the window, and a call
that may meet it and another reads the version cell, which holds the one
the run made last: the cell follows the order the run takes, so the
answers follow CRuby's handler order without the record knowing it. A
call in the load, whose window ends with the load, never meets such a
version.

A block's entry also holds the record's reading of its call
(`elab_block_runs`) and whether it runs where its call does, read again
at each ask. `elab_runs_later`, `elab_in_unplaced_block` and
`elab_in_kept_block` are one query that climbs the bodies around a node
(`lr_in_body`); `elab_blocks_run_at_once` climbs them on its own, up to a
def. `lr_version_order` reads a method
body's `from` for the versions it makes. `--dump-load-record` prints each
extent and where the post-load phase starts (`extent` and `post` lines,
test/load_graph/record_extents_main.rb).

### Versions bodies make

A keyword `def` with no point (in a method body, a lambda, a block or an
exit handler) is a version of the history it writes (`lr_body_versions`):
`def self.m`, `def K.m` and `def m` take the class the record resolves for
their frame (`lr_body_frame`, `lr_body_target`); the dump marks it
`in def#N` with its body's `from`. Only a public def is such a version,
and only of a name the program makes no entry of that the call plan cannot
answer next to it (`lr_body_def_version`, over `lr_body_forms`). The scope
is the name, program-wide: the plan answers no other entry as a body's
version yet, and once a name has versions its readers (the entries' arms,
the poly arms, the presence readers, `respond_to?`, `method_defined?` and
the list readers, the hooks: "Readers of a body's versions" below) ask the record's by-name index, which holds
every history of the name in every class on both sides, and read them as
versions only. The entries that bar it are, anywhere in the program, a def
that is not public where it runs, a def, an alias or an undef that lands
on main (Object's private method, whatever word takes it, which the plan
answers over main's own path, not as a version; a `module Kernel` def
hoisted to the top level is Kernel's and does not count), a visibility
word naming the name, an entry a literal send makes
(`send(:define_method, :m)`: Spinel makes no method of it, so a version
would hide the method CRuby has; a send of a visibility word is none) and
any other entry not public where it runs; in a body, also every other
entry it makes (an alias, an undef, an accessor, a `define_method`, a
`define_singleton_method`, a `module_function` copy); at the top level, an
alias of a name barred so. A public def, and a public version at the top
level (an alias, an accessor, a `define_method`, an undef), are versions
the plan answers. So the rest a body makes stay unplaced, applied from the
start, a non-public def in a method body stays a definition the record
cannot follow, and a public def of a barred name keeps the same form:
`private :m` after `def m` in a class body in a Proc, a `private def m` in
one class method and a `def m` in another, a top-level `private :m`, a
`define_singleton_method` of the name, or a `private :m` in another
class's body. A version of a body that first runs once the load is over (`from` is
`post`: an exit handler, a block or closure only exit handlers run, an
`--ext-entry` method, a method only exit handlers call) is after every
point of the load, and a window with `post` holds it, so a call in an exit
handler or in a method one runs reads the cell. A version of a
body the load runs has its order against a point (`lr_version_order`) from
its body's extent:

- after the point where the point comes before `from` and `from` does not
  run again before it, and after every point for a body that never runs;
- before the point from the statement after `sure` on, where nothing else
  of the history runs after `sure` (`lr_alone_after`): a body one placed
  statement runs once takes that statement's point, so the calls after it
  are direct, and so is a call in that statement that runs
  after the call that runs the body (`p K.setup, K.made`, `lr_run_order`);
- not known otherwise, since the body may run any number of times: a call
  there reads the version cell (the entry).

The order is asked while calls are bound, and `lr_rta` binds calls while
it builds the extents. Until `lr_extents` is done, a body's version
answers "not known" (`LoadRecord.ext_building`), so no binding reads a
half-built extent; `lr_run_points` aborts if a run point is read while the
extents are being built.

A hook a body defines (`def self.method_added`, `def self.inherited`,
`def self.included` and the like, in a class method or a `class << self`
there) may be live when the walk fires hooks: `lr_hook_live` and
`lr_fire_inherited` count one whose target is on the class's chain
(`lr_hook_body_def`), so the walk grafts the hook's call. Which body that
call runs is the call plan's: it is bound against the versions after the
walk, and where none of the program's is live at the call, its entry's
else arm is the core's no-op hook (`VK_NOOP`, `is_definition_hook_name`),
not `super`, which from a subclass's entry would reach the superclass's
version. A class-side def a body makes fires `singleton_method_added`
after it on each run of the body (`lr_body_hook`), when the class's chain
holds a version of that hook.

### What an answer means

The queries answer `ELAB_YES`, `ELAB_NO` or `ELAB_UNKNOWN` as of a point
`p`. Entries after `p` do not count. An entry before `p` that may not
have run, and one in another region, make the answer `ELAB_UNKNOWN`
unless a sure entry decides it; for a method or a mixin, so does one
after `p` in a body around `p` that runs again (`rep`). A `p` with no point (a method body) asks as of the end of the
load phase, where only a sure entry of region 0 counts.

`ELAB_UNKNOWN` is not `ELAB_NO`. A reader acts on the answer it can
prove and takes its fallback on `ELAB_UNKNOWN`: `call_target` keeps the
class table's method, and the nil facts count a variable as written only
on `ELAB_YES`. Before the record is made the queries answer `ELAB_UNKNOWN`;
while the walk records it, a point up to the statements recorded so far
is answered (`lr_ready`).

## The queries

| Query                                                           | Answers                                                                                                                       |
| --------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| `elab_point_of`, `elab_point_from`, `elab_point_done`           | a node's point, the point it can run from, the point its statement completes at                                               |
| `elab_point_may`, `elab_in_method_body`                         | whether a node's statement runs only under a condition; whether a node is in a method body                                    |
| `elab_cmp`, `elab_order`                                        | two points', or two nodes', order: `PROG_BEFORE`, `PROG_SAME`, `PROG_AFTER`, or `PROG_UNKNOWN` across regions                 |
| `elab_const_at`                                                 | what a constant name read in a node's lexical scopes resolves to at `p`: those scopes' own, then the ancestors, Object        |
| `elab_const_own_at`                                             | what a class's own constant of a name was before `p`, as `class X` asks it                                                    |
| `elab_class_ref_at`, `elab_header_class`                        | the class a constant reference names where it stands; the class a `class` or `module` header opens                            |
| `elab_written_before`                                           | whether a non-nil value was written to a global, main's ivar or a top-level constant before `p`, on every path                |
| `elab_method_at`, `elab_method_vis_at`, `elab_mixed_at`         | the version of a method live at `p` through the ancestors as they stood there; its visibility; whether a module is among them |
| `elab_version_end`, `elab_method_over`, `elab_version_captured` | where a version is replaced; the version live over an interval; whether a version may run outside its interval                |
| `elab_entries`, `elab_class_surface`, `elab_class_mixins`       | what the registration applies as the load phase ends: entries by kind, each name's surface, the mixins                        |
| `elab_class_bodies`, `elab_defines`, `elab_def_applied`         | a class's bodies in run order; whether a body defines a name; whether the load phase applies a def where it stands            |

`prog_order` and `prog_sort` (`src/analyze_program.c`) order nodes by the
record too, by the rank of the top-level statement holding each node.

### Registrations from the record

The class tables are filled from the record's entries, so a form the
record holds needs no walk of its own:

- each method scope of a `def` or `define_method` takes its class side
  from its history (`lr_def_owners`): the frame it is written in and its
  receiver decide it, whether or not a load point applies it;
- `register_attrs`, `register_aliases` and `register_undefs` take
  `elab_entries` of their kinds, in the order the load phase makes them;
- `register_method_visibility` takes `elab_class_surface`, each name's
  visibility as the load phase ends;
- `register_includes`, `register_extends` and `register_prepends` take
  each body's mixins (`elab_class_mixins`, `LrMixin.open`,
  `LrMixin.applied`); `register_include_attrs` asks `elab_defines`.

An entry applies (`lr_stmt_applied`) when no run-time condition in its
class body stands before it and it is in no iterator's block or `begin`
body with rescue clauses there, counted from the frame. A constant
condition is folded, so `attr_reader :x if true` applies. An entry under
a run-time condition in its class body gives its name its visibility
only (`elab_class_surface`). An entry with no point does not apply,
unless its body is one the walk does not place. A mixin takes the test
as the record is made (`LrMixin.applied`). A mixin in a branch the scope
walk proves unreachable leaves the existing method history in place.

### Readers of a body's versions

A call of a method that a method body defines takes the entry of the
method's versions before the body has run (its version is late,
`lr_late_make`). A `respond_to?`, `defined?` or `method_defined?` (and
`public_method_defined?`) of that name answers as the call there would
find it: where a call of the name at the reader takes the entry
(`cplan_presence`'s `PR_CELLS`), the call plan gives the reader's answer
for each class its receiver may be (`cplan_presence_list`): yes, no, or
the version cells. That covers an instance (a typed receiver, nil, and
self in an instance method, which a receiverless `respond_to?(:m)` or
`defined?(m)` asks), a boxed receiver, and a class value's
`method_defined?`; codegen renders the list as one test of the
receiver's class. Each reader asks the entries a call at its point asks
(`presence_asks`) and keeps the cells it reads, in the form it asks them
(`presence_reader_side`: a `respond_to?`'s public versions, or every
visibility with `true`; a `method_defined?`'s as a list asks): a test
that reads a cell no reader kept is an internal error.

A `respond_to?` of a literal name, with include_all or without, on a
value that may be a class asks the class side of each class it may be
(`cplan_class_value_presence`): a class whose class method a body
defines answers by that method's version cells, as the builtin does
where none is live; one the record proves has no class method of the
name live at the reader answers as the builtin does; any other answers
from the table. A module's instance method a body defines answers so
on a class that extends the module. Codegen renders the plan's arms as
one test of the value's class (`class_value_responds`).

A list of a class's own instance methods on a constant
(`C.instance_methods(false)` and its `public_`, `protected_` and
`private_` forms, `list_cells_shape`) is a reader of each name a body
may define there, on the class's own side (`CT_OWN`): the list holds the
name where one of its cells says it is live (a body's version is public,
so a list that shows public names). It asks the class object, live from
its header on, where no instance may exist yet (`ct_version_at`).

A reader that answers from the final tables instead keeps the table's
answer for the calls of the names it may read: such a version is not
late (`elab_body_reflected`), so a guard and the call it guards answer
alike. Those are a list that includes inherited names
(`K.instance_methods`, `instance_methods.include?`), a list on a
module (`M.instance_methods(false)`), a list on a receiver that is not
a constant, and every reader of a program that defines
`respond_to_missing?`; there a body's def is listed, and answers
`respond_to?`, before it runs. A module's def such a list may read is
the table's on a class that extends the module too, so the class value's
`respond_to?` answers it from the start. Only list readers of the same
side count: a class-side list (`K.singleton_methods`) leaves an instance
method's versions to the cells. A body that makes a definition the
record cannot follow (a `private def`) is listed whole from the start.

A reader called inside a body (a class method that defines `m2` and then
calls one that asks `respond_to?(:m2)` or calls `m2`) reads what that
body does: the record follows a def it holds as a body's version
(`lr_body_held`) instead of taking the call for one that may define
anything (`lr_callee_defines`), so the reader's call there takes the
entry, which finds the def once it has run. These still make the body's
calls opaque, and the reader answers from the start: a reader reached
through `super`, a literal `send` or `public_send`, or a Proc kept in a
constant; a reader that makes a lambda, or takes a block and calls it; a
body that passes its block on to the reader (`look(&b)`); and a body that
calls itself.

## Freshness and cost

The map of points is a function of the tree as it stands. `elab_fresh`
rebuilds it when the node table's `count` or `version` differs from the
map's, except while the registration's walk makes it or the call plan's
frame is in. `nt->version` moves when a node is added (`nt_new_node`,
`nt_clone_subtree`), retyped or reset, a string field is rewritten
(`nt_node_set_str`, `nt_set_str`, `nt_node_swap_str`), or nodes are
swapped or renumbered. An edit of an int, ref or array field alone
(`nt_node_set_int`, `nt_node_set_ref`, `nt_node_set_arr`) moves only
`nt->edits`, so the map does not see a relink until one of the others
follows.

A rebuild walks every file again and climbs every node to its statement,
over the parent map, which rebuilds on the same test (`comp_parent_map`).
A pass that appends or edits nodes between its asks rebuilds the map at
each ask, which makes the pass quadratic in the program. Such a pass
makes its asks before its first edit or, for the method a call reaches,
asks `call_target`, which reads the call plan's frame.

The call plan's frame is the map `call_target` reads. The first time
`call_target` binds a call to one of a method's versions, `elab_plan_enter`
copies the map and its parent map, and every later ask reads that copy
(#10422). A node appended since the copy is placed from where it was first
linked (`nt_origin`, `lr_plan_place`), as a rebuild would place it, and a
clone where the node it copies stands (#10490). A node the copy cannot
place is asked of the map as it is. So a version is decided from the
facts the record had for the call when the plan first asked; a pass that
moves a call between bodies does not change it.

To check a change for this cost, compile a large program with
`--timing` before and after it: the `an_elab_points` line counts the
map's `builds`. A count that grows with the program's size is a pass
asking the map as it edits. `SPINEL_ELAB_CHECK=1` reports where the
plan frame's version of a call differs from the map as it is ("the call
plan's versions").

## Requires and the two models

[require.md](../require.md) describes `require` as a program sees it.
The record is made under both load models.

Under `--load-model=inline`, the default, required files are spliced into
the one program text, which has one root. The walk is the same over that
text: one region, and a statement a desugaring placed in a body has its
point where it stands, whatever its id.

Under `--load-model=graph` each file is a root of the node table
(`nt_root`), and each require a `LoadNode` naming the file it loads
(`target_file`). The parse's load graph places each file at the
statement that first loads it (#10305): `prog_ordered_load` answers the
file a top-level load runs, and the walk enters it right after that
statement, in region 0. A file a require nested in a statement loads,
which `prog_settle_files` placed at that site, is entered where the
walk reaches the require (`prog_site_places`): its statements share the
rank of the top-level statement holding the require, and run as the
require does, `may` or `partial` with it. A file neither places (for
example one loaded only in a method or in a block kept to run later) is
walked after them as a region of its own, whose points are
`PROG_UNKNOWN` against any other region's. `--dump-load-graph` prints the
files and what each require resolves to. What the graph model does not
compile it refuses by name (`prog_refuse_loads`, `prog_refuse_order`).

How the load stage's lowerings (`lower_run`, `src/analyze_lower.c`) and
splices place the nodes they make in the record is described when that
work lands. So is how the record answers implicit calls, the methods the
language calls for a node.

## Checks

- `SPINEL_ELAB_CHECK=1` makes the record check itself as it compiles,
  and reports on stderr in `spinel-elab-check:` lines, each family's
  count at the end (`family="..." asked=N differ=M`): `prog_order` and
  `prog_sort` against the record's order, the map the walk made against
  a rebuilt one, the record the walk made against one made after it,
  each class header's entity against the class registration opened,
  each def's owner, the class tables' readers, writers, undefs, aliases
  and visibility against the record's surface, and each call's version
  in the plan frame against the map as it is.
- `make load-graph-test` (`tools/load_graph_check.sh`, part of
  `make test`) compiles each test/load_graph program with an `.expected`
  under `--load-model=graph` and compares its output, and compares
  `--dump-load-record` of each with a `.load-record` against that file.
  A change to the record's points or histories updates those files.
- `ruby tools/load_record_oracle.rb PROGRAM...` runs each program under
  CRuby with a tracer that logs what its load phase defines (class
  bodies, methods added, removed and undefined, constants set, mixins)
  and compares it with the record: an event only CRuby ran, a sure entry
  CRuby did not run, two events in the other order. `--inline` is for a
  program of one file under the inline model.
- `--timing` prints the map's builds (`an_elab_points`) and the record's
  own cost (`an_elab_record`).
