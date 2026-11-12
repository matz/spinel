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
method body, a lambda or a block kept to run later has no point and is in
no history. A class body that is no statement of the load phase (in a
lambda, in a block kept to run later, a class builder's block in an
expression) is its class's body all the same: its definitions are
recorded after the placed bodies, `unplaced`, so the registrations apply
them but no query at a point sees them (`lr_record_unplaced`,
`lr_body_defs`).

Some answers wait for the whole record. A file in a region after region 0
is walked after everything the entry does, so the answers recorded on the
way (a mixin's module, a form's "runs program code", an alias's
visibility) are asked again once all of it is made (`lr_settle_regions`).
A class builder's class (`X = Struct.new(:a) do ... end`, a local's too) is
made where the walk reaches its write (`record_write_class`), so its block
is recorded as that class's body like a header's.

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
as the record is made (`LrMixin.applied`).

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
