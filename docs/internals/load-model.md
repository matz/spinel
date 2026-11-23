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

`--dump-load-record` prints the record and stops the compile. It reports
`unread files: possible` when the program may load a file the compiler
did not read. For test/load_graph/record_main.rb, part of the points reads:

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
  first run on (`rentry`), and no earlier than the first statement one of
  the loads that may run its file may run on, past the body around the
  load (`lr_region_time`, after every other body's extent): a require in a
  method starts where the method does, one no run reaches leaves the
  region `never`. A require of a set of files (an open one, or one of
  candidates) may load any of them, and a file an autoload names keeps its
  entry, as the constant's first read loads it; a node of a body that has no point of its own
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
  or under a rescue. A region is sure where a require of its file alone
  surely runs (`lr_load_sure`): a top-level statement of region 0 by the
  tests above, or a statement that runs whenever the method body or the
  in-place block it stands in does, from that body's `sure` (a block's: past
  the last point of its call's statement, since the code nested there may
  run before the require). A require of a set of files runs every one of
  them only where the set is one file per word of the `each` or
  `each_with_index` block over a literal Array of Strings it stands in
  directly (`%w(a b).each { |f| require_relative f }`, the parse's
  `per_word`, `prog_load_per_word`), that block is sure and it has no early
  exit (`break`, `next`, `throw`, ...). A set a path pattern names
  (`"lib_#{name}"`, every file of the program its literal head and tail
  match) loads one of them, so it is never sure. A require runs its file once whatever
  other requires of it run, so a `load` of the file, which may run it
  again, leaves it unsure and `many`. A method of main's is sure, as one of
  a constant receiver, at a receiverless call of a top-level statement of
  main's when the program has one def of the name and no builtin method
  takes it (`lr_rta_bind`). An in-place block on a literal
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
`in def#N` with its body's `from`. So is a visibility word such a body
runs with literal names (`private :m`, `protected :a, :b`, `public [:c]`,
`private def m`, an Array only as the word's one argument; `lr_body_word`),
a version of each name it takes, where
the tree spells the class self is there (a class method's body, self's
eval blocks, and a `class << self` it opens, whose word names the class
side: `lr_body_word_frame`). A class-method word in a class method's body
(`private_class_method :m`, `public_class_method [:a, :b]`,
`private_class_method def self.m`, or with String names,
`private_class_method "m"`) is one too, of the class side's
history, and so is a literal send of either word or of an instance word
on self (`send(:private, :m)`, `vis_word_call`); a class-method word's
`def self.m` is the def's version followed by the word's, which runs once
its argument has. An instance word takes its def only where self at the
word is a class or module: in an instance method's body or a
`define_method` block the word raises NoMethodError and the def stays
public (`lr_body_def_word`, asking `lr_body_frame` for `LR_FS_OBJECT`). A
call before the body runs of a private def it makes finds no method: the
plan binds it to the version's face entry (`ver_lone_closed_late`). A
class-method word whose first name no method of the
program's has, on either side, and no builtin class method answers
(Object's, Kernel's, Module's, Class's: `cplan_class_builtin_has`) raises
NameError where it runs and changes nothing (`lr_cside_word_absent`). A body's `module_function :m`
is two versions: the instance side's private one, as `private :m`'s, and a
copy on the module's singleton (LR_COPY, `LrDef.body`) whose scope is the
def live where the word runs, the last def of the name ahead of it in the
same statements unless a statement between may define the name again
(`lr_between_redefines`, asking `lr_node_defines` with the name watched),
else the one version the body's window meets
(`lr_body_copy_scope`); where the window meets two or more the copy has no
one body and `mf_copy_entries` refuses it, and where the program defines
no method of the name it makes none. A name the module object has a
builtin method of (Kernel's private `puts`) bars the word, as a class-method
word's on one does. CRuby's word makes each name private in turn and
then copies them: a name no method has raises NameError there, with the
names ahead of it private and no copy made, and `module_function [:a]`
makes the Array's names private, as `private [:a]` does, then raises
TypeError (`[:a] is not a symbol nor a string`) and makes no copy; an
element that is no name raises TypeError for itself where private's
would. A method the module has is one CRuby's lookup finds from the
module's origin, past its own prepends, and then from Object: an entry
the record holds of the name on the module, a module it includes
(through another's include, or that one's prepend, too), Object, Kernel
or main, or a builtin method of a builtin module among those (an
included Comparable or Enumerable, by its builtin id) or of Object's
table (`elab_module_has_method`), not a def only another class has, nor
the module object's own methods (`name`). Every visibility word is read
so (private, protected, public, `private_class_method`,
`public_class_method`, as `module_function`): CRuby changes its names in
order and raises NameError at the first that no method has where the word
runs, so the record takes the word as run for the names ahead of it
only (`lr_word_reach`; module_function makes its versions of those only), where the word's statement is partial (in a rescued
begin or a block: a word that runs whole ends the program where it raises),
and of every name where an argument is no literal name (arguments are
evaluated first). The test is the record's own at the word's point
(`lr_word_has`: `lr_search` for a method the program holds, a def after the
word is no method yet; the builtin fact only for a name no entry of the
program's own on the class answers), so a name defined, aliased, included
or reopened after the word is no method yet. A method counts only where
the record can prove it ran: a def, a `define_method` or `attr_*` with a literal
name, or an alias of one that is itself proven at the alias, on a straight-line
path from the main file's top level (statement lists, class and module
bodies, a `begin` with no rescue or ensure clause, a def a visibility word
takes) and reached through superclass and include or prepend links that are
too (`lr_path_sure`, `lr_link_sure`), none through a module the class itself prepends (the lookup starts at its origin), and Object's and the top level's
entries only where Object is on the class's chain as the record holds it (a
BasicObject subclass has none). A `def` among the names counts as one name, as
the emitted word counts it. Everything else is unknown: a def in a
block, a condition, a loop, a rescue modifier or a rescued begin, a Struct's or
Data's member, an entry no point places or a method body's, a chain the record
does not hold to its end. The reach stops at an unknown name as at a missing
one: the names from there on keep the answer they have without the word, which
is how their entries read where the word is partial (`LrDef.cut`: the word
names them, and has not changed them). A def among the word's arguments is
a method the word names (the arguments run first, `private(:m, def m; end)`)
and the name of one element of it, in the reach; where the word runs
(`lr_def_arg_runs`) the def is made whenever the statement is reached, with the
word's visibility, as the word is (`private(def a; end, :missing)`). A
module_function copies the names only where it makes every name private
and is given no Array (`copies`); the word writes their visibility cells
before it raises (`emit_vis_word_absent`, which asks the call plan whether
a name is a method), in a module body too. In an iterator's block or a begin body with rescue
clauses, where no statement ahead of the word in its frame may raise and
skip it and no condition holds it, the record, with no run-time cell for
the word's visibility, takes the word either to have run or not
(`lr_word_runs`; a word with an argument that is no literal name is left
as any partial statement's). A word with Kernel's `raise` or `fail` as an operand (`module_function :a,
raise("stop")`: the operands run before the word does; the program defines no method of the
name, and a raise in a block, behind `&&` or in a rescue modifier is not one) never finishes,
and none of its names changes: the record holds it as it holds a form main's `protected` leaves (`lr_form_never_runs`),
and codegen runs the operands only. The position walker marks each statement in iterators'
blocks (`ElabPos` and `LrStmt` `skip`, `unsure`): skipped where a block
around it surely runs no time by the run count the record's iterator
rows give (`[].each` on a literal, `0.times`) or a statement ahead of it
in a block surely leaves it (a `break`, `next`, `return` or `raise`,
`lr_stmt_leaves`); unsure where a block around it is not known to run at
least once (`n.times`, `loop`, `each_slice`, a `&.` call) or a statement
ahead of it in a block may raise or leave it. A word in a block that
surely runs (`[1].each`, `tap`, `2.times`) has run, and those names are
its applied versions though the statement is partial; a skipped one has
not, as any partial statement's; an unsure one keeps every name's
versions and copies (`lr_word_reach`), with its statement as any partial
statement. In
a class, or in a `class << self` body, a module's too, whose self is a
singleton class (`elab_word_singleton`, which gives the depth of a
`class << self` body inside another, one `#<Class:` each in the
message), which has no `module_function`, the word raises NoMethodError,
a bare one NameError as any vcall that finds no method does
(`emit_missing_raise`), and changes nothing. Not a
`def self.m` argument, which names a class method the word looks up
among the instance methods: that word raises NameError where it runs, as
any word naming a method the class has none of does, in a statement or
for its value (`emit_vis_word_absent`). In a class body a builtin method
the class has counts, its builtin root's or an included builtin
module's; a method body's word on one raises still. A def's version holds the
visibility it has where it runs (`lr_body_def_vis`: a word that takes
it, a bare word before it in a class body's statements, private for
`initialize` and its kin); a word's version holds the visibility it sets,
which the plan reads where the word runs (`lr_version_vis`, from the
version's visibility cell where only the run decides). Only a def or a
word of a name the program makes no entry of that the call plan cannot
answer next to it is such a version (`lr_body_def_version`, over
`lr_body_forms`); a visibility word placed in a class body is an entry of
its name's history with a point of its own, which the plan orders before the
body's version, so it is no such entry and bars none, but for the name its
reach ends at in a partial statement (one no entry of the program's places
at the word, which a body's definer may have made by then): that stays
barred, so the body's definer of it keeps the refusal it has there; a
class-method word (`private_class_method`, `public_class_method`) keeps
barring every name it takes, as the class side's faces read it. So is a definer call on self that stands in a
method's body, a def's or a `define_method`'s or
`define_singleton_method`'s block's, with no class body between
(`lr_body_call`, `lr_in_method_body`; one in a block a class body hands a
method that yields keeps its form, since that block is spliced into the
class body's statement): a `define_method` or `define_singleton_method` with a
literal name and a literal block, a version of the history of the class
self is (the class side's for `define_singleton_method`; `lr_call_side`),
whose scope is the block's (`ct_def_version` maps it as a def's), and an
`attr_reader`, `attr_writer`, `attr_accessor` or `attr` with literal
names, a version of each reader and writer it makes, whose method
`register_attr_versions` makes of it (`def x = @x`): no slot of the class
answers it from the start. A definer call bars its names (keeping the
form it has where the record holds no body versions) where the tree does
not spell the class self is (an instance method's self is an object, and
so is a `define_method` block's, `LR_FS_OBJECT`; a
`define_singleton_method` block's self is its receiver), on a module, in
a class method a call may run on a subclass (`lr_body_self_unsure`; in a
`define_singleton_method` block, `lr_body_dsm_unsure`), for
a definer the program's own method of its name may answer
(`an_user_method_reaches`), for a name not public where it runs, and on the instance side where the program
gives an object a singleton method or module (`LrBodyNames.osg`: the
class `register_singleton_defs` makes of it, after the record, reads the
class's methods from the start) (`lr_body_call_barred`). A
`define_method` or `attr_*` on self in a `define_method` block of an
ordinary class's instance method (`elab_body_self_object`), which no
method of the program's of its name nor a `method_missing` answers, is
the plan's NoMethodError (`an_object_definer_missing`, `cplan_missing`'s
`CM_NOMETHOD_SELF`, the object described at run time), and the walk
registers no method for it. Self there is taken from the tree's frames
(`lr_body_frame`): a class_eval-family block and the parentheses a
value-read one is spliced into (`self_rebound`) make self their class. A
`define_singleton_method` there, whose object no `.new` traces, makes no
method; the call plan refuses one that runs whenever the block does
(`elab_runs_with_body`) where a written call of its name may reach that
object (`cplan_instance_call_named`, `an_call_may_reach_instance`: by
its receiver's class facts, as a dynamic call's kill asks them, over the
classes that mix a module in and a builtin superclass's values, or by
self's class; a call in a method nothing reaches reaches none) and the
program has no `define_singleton_method` of its own. A block that reads
or writes the locals of the def around it closes over them (`Scope.env_host`,
`scope_env_link`): a lookup of one of those names in the block's scope
answers the def's own `LocalVar` (`scope_local`, whichever node asks, so a
desugar's clone or a pattern's binding needs no filing, and a reader that
indexes a scope's locals takes the local's own scope), `mark_env_captures` makes
them heap cells as a proc's captures are (the locals of a def, and the
parameter of an iteration block around the definer, with its loop slot kept:
`LocalVar.cell_shadow`), the definer's statement builds the capture struct
`_proc_cap_env<scope>` over those cells and stores it in
`gv___spinel_env_<scope>` (`emit_env_store`, one global per scope the method
pass emits, marked by `sp_mark_user_globals`), and the method it defines
loads that global into a rooted local `_cap` as it starts and reads the cells
through it as a proc body does (`emit_env_load`, `g_cap_struct`): a run of the
definer that comes while a call is in progress leaves that call its own
environment. A call that reaches the method before its definer has run, by a
dispatch that reads no version cell, finds no environment and raises
NoMethodError. A default of the block's parameters that reads one of those locals is spelled at the call site in the method's
environment (`env_bind_begin`: one statement expression binds it ahead of the default's own code, the statements
it hoists included, and raises the NoMethodError there where the definer has not run). A method with an
environment is never spliced into a call site (the yield-inlining pass skips it), so its prologue always loads it. The call plan refuses a block that yields to the method's
block, writes one of them through a regex's named capture, is a definer inside another definer's block and reads or writes a local from outside its own block (the method's or the outer block's: only a def's locals have cells), closes over the method's locals where a run may define the method on
another class or object than the one it is registered on
(`elab_body_call_owner`, set with the bars in `lr_body_forms`:
`lr_body_owner_fixed`), or closes over the method's locals where the program aliases its name
(`an_aliases_name`: the alias keeps the closure of one run), or a local no closure cell can hold (`cplan_env_cells_ok`, over `repr_cell_capturable`), where the
program reaches the name it
defines (`an_name_reached`: a written call, or a Symbol or String
that may run it, `elab_name_spelled` without presence queries), and a barred
define_method in a def's body whose method the walk registers from the
start where the class or its chain answers the name, or a Symbol or a
String spells it (`cplan_define_method_early`), or one with a Proc or a
Method for its body (`cplan_body_definer_gap`). It refuses a definer
call's version too where a reader of its name does not read it
(`cplan_call_version_unread`): a method list or a reader of a name only
the run tells (the `elab_body_reflected` exception, which keeps a body
def's version off the late bit, and takes in every presence reader of
the name where a `respond_to_missing?` is defined), a `method_missing`
on the chain, a Method's maker of the name (`elab_method_captured`), a
`method_added` or `singleton_method_added` hook of the program that the
definition would fire (`elab_definer_hook_live`), a name Ruby calls by itself (`to_s`, `<=>`, `coerce`), keyword parameters,
a read of the call's own value, a class method a `define_method`'s or
`define_singleton_method`'s block makes, and a subclass's method that
supers into it. Each answers from the method
tables, which hold the block's scope from the start; a body def's
version meets the same readers and keeps master's answer there.
The scope
is the name, program-wide: the plan answers no other entry as a body's
version yet, and once a name has versions its readers (the entries' arms,
the poly arms, the presence readers, `respond_to?`, `method_defined?` and
the list readers, the hooks: "Readers of a body's versions" below) ask the record's by-name index, which holds
every history of the name in every class on both sides, and read them as
versions only. The entries that bar it are, anywhere in the program,
`initialize` and its kin, a def, an alias or an undef that lands
on main (Object's private method, whatever word takes it, which the plan
answers over main's own path, not as a version; a `module Kernel` def
hoisted to the top level is Kernel's and does not count), a visibility
word naming the name that is no body's version (one at the top level or
in a class body the load places, one with a name only the run spells,
one whose frame the tree does not spell), an entry a literal send makes
(`send(:define_method, :m)`: Spinel makes no method of it, so a version
would hide the method CRuby has; a send of a visibility word is none);
in a body, also every other
entry it makes that is no version (an alias, an undef, a barred definer
call, a `module_function` copy); at the top level, an
alias of a name barred so. Six more bars keep a body's word, or its
non-public def, in the form it has where the record holds no body
versions, until the readers it needs read the version cells; each says
in the source what drops it. While the walk has not opened a class, an entry
in it bars, because the bars read its class as not recorded; once the class
opens, the entry is asked again, once, at the first read after it
(`lr_bform_live`). They ask what one walk over the tree
gathers (`lr_body_names_read`), so each ask is a lookup:

- a word on a name the class does not define itself on the word's side
  (`lr_body_word_barred`): an inherited method keeps its own visibility
  until the word runs, where a version would be the word's from the
  start. A name nothing defines has no method a reader may count: the
  word raises NameError where it runs, and a version would make the
  readers count it. A word in a module's body on the module's own method
  is no bar: it is a version of the module's history as a class's is, and
  every copy reads it. An include, extend or prepend copy holds the
  module's def versions, whose arms ask the module's visibility cell the
  word writes: a chain's face entry (`ct_face_for`), a late layer's face
  entry and a guard entry's arm ("Late layers of a class's chain" below),
  and a protected version, closed to a caller whose class does not
  include the module (`ver_protected_closed`). A module's own presence
  readers (`M.private_method_defined?` and its kin) read the cells of the
  histories on the module's chain, which a call guard over them writes
  (`VerGuard.vis`, `pres_module_cells`): no entry stands over them, as no
  call runs on a module's instance side. Its own lists
  (`M.private_instance_methods(false)`) test the visibilities they show.
  At a load point that surely follows the word, a class's or a module's
  presence readers and own lists take the record's visibility
  (`cplan_program_method_vis`, `cplan_own_list_vis`);
- any body word in a program that defines `method_missing` or
  `respond_to_missing?`: the entry's else arm and the presence readers
  answer from the table there;
- a word or a non-public def in a body whose own calls may reach a send,
  a super, a yield, an eval or a definition form, itself or through a
  method of the program it calls (`lr_body_followed`, read from the tree
  alone: `lr_body_defines` with `LR_DEFINES_TREE`, where every def and
  word counts as followed, so the answer is the same while the walk
  records and after it): the readers those calls reach answer from the
  start, where a version would be its own visibility before CRuby has the
  method;
- a word or a non-public def in a class method a call may run on a
  subclass (`lr_body_self_unsure`): where a class header's superclass
  constant names the class or one the tree does not fix, or a superclass
  no constant names, a `Class.new` with a superclass, or a `dup` or
  `clone` of a constant may make one, and a call of the method is not on a
  constant or in a class body that is the class itself. The classes are
  read from the tree alone, so that the walk's first ask answers as the
  finished record does: a header is its lexical path (`lr_body_key`), and
  a constant is the class whose path every header of its name has, where
  no write other than a header may bind the name anywhere in the program
  (`lr_body_ref_key`: a constant write or target, a `const_set`, a
  `remove_const` or an `autoload` naming it, reached or not; with a
  computed name, a send of those, a `const_missing` or a String eval, no
  constant is fixed). So a call on an alias of a subclass, on
  `K = Class.new(A::K)`, on a constant two classes are named by, on another
  receiver, on self in a method or a block, a send, a Method, a
  block-pass symbol, an alias, a `super`, or a send or a Method maker of a
  name the tree does not spell anywhere bars the word. The word acts on
  the class the call runs on, and the record places the version on the
  class the method is written in;
- a class-side word (`private_class_method :m`, `public_class_method`,
  either sent, `send(:private_class_method, :m)`, or a word in a
  `class << self` body a class method opens) or a class method's
  non-public def, where a reader that reads no class-side version cell
  names it (`lr_cside_unfollowed`): a list of an object's methods the
  cells do not answer (one that is no class's own list on a constant,
  `list_cells_shape`), a `respond_to?` they do not answer (on a
  constant or self one of no `respond_to_cells_shape`; on another value
  one with no literal Symbol and include_all, `respond_to_literal_shape`,
  whose class value's arms read no cells), a `public_method` naming it
  other than one with a Symbol on a constant, on self or with no receiver
  (`capture_cells_shape`), a send of one of these readers
  (`K.send(:respond_to?, :m)`, which reads no cell), or a call or a send
  of it with a block, which binds no face entry. Those readers answer
  from the table, so a version there would let a guarded call raise. The
  others read the cells: a call or a send of it on a constant, on self or
  on a boxed value takes the class side's face entry, whose arms raise
  for the visibility the run gives the version (`ct_face_for`, and
  `cplan_cmethod_arm` for a boxed value's arm), a `respond_to?`, a
  `defined?` and a class's own list read the presence cells
  (`cplan_cmethod_presence`; a class value's arm of a `respond_to?` on
  any other value too, `cplan_class_value_presence`), and `method`,
  `singleton_method` and `instance_method` find a method of any
  visibility. Each reader asks the entry a public call takes, whatever
  its receiver (`ver_cells_reader`), so the words write the cells for it,
  and keeps the cells of the visibilities it counts (include_all or
  public only), none where they fix its answer at its point
  (`ver_cmethod_cells_keep`).
  A `public_method` of a private or protected method raises NameError
  where the record tells that visibility at the capture and no visibility
  word the record does not place or holds no entry of may change it
  (`cplan_capture_closed`, `elab_method_vis_unplaced`); where a body's
  word may have, it tests the presence cells first (`CP_CLOSED_RUN`,
  `emit_capture_closed_cells`), and is refused where they cannot tell
  (`CP_CLOSED_UNTOLD`); elsewhere it keeps the method;
- a protected word or a protected def in a method body, where a call on a
  receiver other than self (or a `public_send`) in a block whose self
  nothing tells names it (`cplan_block_self_untold`: a block handed to a
  method of the program, a lambda, a `define_method` block, an
  `instance_exec` on a local; asked of the tree and the names the program
  defines alone). The call plan takes the scope's self as such a call's
  caller, so a protected version would close the call where the block
  runs with an instance of the class; the form with no body versions
  keeps the method's visibility there. A block the tree tells the self of
  (an `instance_exec` on `K.new` or on self, which runs with the self of
  the block around it, an eval block on a constant,
  a bare `new` in a class method of a class nothing subclasses, a block of
  a builtin that keeps self) is no bar.

A class side's accessor (an `attr_reader`, `attr_writer` or `attr_accessor` in
a `class << self`, or on `singleton_class`) whose visibility a method body's
word sets is a method, as it is in CRuby: its history holds a version of the
word, so `elab_replaced_attrs` lists the accessor's version beside the ones a
def replaces, and `register_attr_versions` makes the method the accessor is
(`Scope.accessor_of`, found by name) where no def replaces it, as an
instance-side alias of an accessor already is. A class-side alias of an
accessor whose visibility such a word sets is the method made of the accessor
(`alias_accessor_method`, with the class-level ivar it reads and a writer
writes). The call plan binds a call of such a name to that method
(`cplan_sg_version`: the slot stands in only where no word sets the name, or
where the bound scope is a subclass's own accessor, not a method made of one),
so every reader of a class method's version cells reads it: the static raise
at a placed point, the entry where only the run tells, the boxed class arms,
`respond_to?`, `defined?`, `send`, `public_send` and `method`.

A public def, a body's version as above, and a public version at the top
level (an alias, an accessor, a `define_method`, an undef), are versions
the plan answers. So the rest a body makes stay unplaced, applied from the
start, a non-public def a bar keeps stays a definition the record cannot
follow (public from the start), and a def of a barred name keeps the same
form: a top-level `private :m`, a `private :m` a class body the load
places runs after a body's version, a `define_singleton_method` of the
name, or a `send(:private, :m)`. A version of a body that first runs once the load is over (`from` is
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
unless its body is one the walk does not place. A visibility word in a
`begin` body with rescue clauses or an iterator's block that surely runs is
taken to have run for the names it reaches (`lr_word_runs`, below).
A mixin takes the test
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
name where one of its cells says it is live with a visibility the form
shows. It asks the class object, live from its header on, where no
instance may exist yet (`ct_version_at`). A method live there whose
visibility a body's word sets where it runs is read the same way: the
word's version cell and its visibility cell decide, and the method's own
visibility where no word has run (`cplan_presence_cells`), not the
visibility the table ends with, so the private form lists the name once
the word has run and the public form stops listing it.

A call, `respond_to?` or `method_defined?` at a point the load places,
before or after a call of a class method whose body runs a word, reads
the visibility the record holds at that point: the method's own before
the body may have run, the word's after a statement that surely ran it
(`elab_method_vis_at`), and the visibility cell where the body may have
run any number of times.

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
record cannot follow (a non-public def a bar keeps, above) is listed
whole from the start.

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

### Late layers of a class's chain

A mixin copies its module's methods into the class's lookup: a prepend's
copy in front of the class's own def, an include's or an extend's behind
it, the stand-in a mixin has in front of a class with no method of its
own (`super_behind`). Each copy is a *position* of the chain. A position
is a *layer* when a call may meet none of its versions live yet (a
version a method body makes, or a def a later statement makes, ahead of
which a call may run: `elab_versions_late`), or the module holds several
versions (a guarded copy, an include's or an extend's hidden copy). CRuby's
lookup passes a module until its method exists, and so does a `super` from
a position in front of it.

The call plan makes one entry for each layer, once (`pos_entry`, kept in
`ver_entries` as `VER_ENTRY_LAYER` from the position's scope): an if-chain
over the cells of the versions' histories, nearest first, its else arm what
lies behind the position: the entry of the next position where that is a
layer too (`pos_next`), else its method, or, where a later prepend guards
the method of the class behind it, that class's guard entry (`pos_guard`),
which tests the prepend's cell first. Where nothing lies behind it, the else
arm is what a call that meets no version made gets: the receiver's
`method_missing`, or NoMethodError. A subclass's own `method_missing`
answers its receivers where the class has none (tested by the receiver's
class), and a class that has one answers every receiver but those of a
subclass that defines its own (the dispatch of the arm). A subclass whose
`method_missing` a method body defines, and every subclass where the call
hands a block (a `method_missing` only subclasses define cannot take one:
codegen refuses it), keeps a version for its receivers, as before the arm
answered: at each position of the layers whose lookup ends so, the version of
that position (its last, pinned), so that a layer's body's `super` reaches the
next position's entry, which pins its own, as the entries of a chain ran
before. The record does not ask the cell of a body's `method_missing` (a
builtin name), so it would be taken for there from the start. Every reader of a position asks that entry, so the lookup behind a late
layer is made once, whatever reaches it:

- a later prepend's guard entry (`guard_entry_make`) calls the entry of each
  guarded copy it arms and of the method behind its arms, testing the
  mixin's cell and then the version's cell;
- a prepend in a class body in front of the class's own def, which the table
  answers, is the class's first position (`pos_front`): the call takes its
  entry, whose else arm is the own def under its hidden name;
- a `super` that reaches a layer takes the same entry (`super_layer_entry`);
  where the layer's cell is set wherever the super's method runs
  (`layer_ran_before`), only its versions' cells are tested, else the guard
  entry's;
- `Method#super_method` writes one Method for each of the entry's arms, as a
  capture of an entry does (`cplan_super_method_target`).

A call that enforces visibility, where a version on the chain takes a
visibility only the run tells (a word in a method body), takes the layer's
face entry (`ct_layer_face`, kept beside the plain one by its face): the arm
of a version closed to the call raises, and the arm of a version whose
visibility only the run tells asks the version's visibility cell first
(`ver_vis_arm`), as the chain's face entry does; its else arm is the face's
entry of what lies behind. A guard entry's arms do the same for the module
version each guarded copy runs (`guard_version_vis`), and the layer a
guarded arm calls takes the arm's face (`guard_arm_face`), so it tests its
own versions; the presence readers read the guard's cells
(`cplan_guarded_presence`), each copy's visibility from its version's cell.

The pin of an entry's arm, and of its else arm, is static: it binds the
method of a class the receiver's class is at or below, whatever the
receiver's own lookup finds (`ct_pin_binds`), so no arm dispatches on the
receiver's class and a subclass's method of the same hidden name is no
target of it. A position in a superclass's part of the chain is the
superclass's: a subclass's `super` into it takes the superclass's entry.

A chain of guarded copies longer than `VER_MAX_ARMS` (64) gets a guard entry
that holds only the first 64 of them: `guard_chain` stops there, and the
entry's else arm is the first scope after them, the 65th copy, run as if its
prepend had run, so the copies from the 65th on are not tested and a late
layer among them is not passed. It is a silent truncation, not a refusal; the
chain would have to be cut into entries of at most 63 arms, each else arm the
next.

What it does not cover: an include or an extend in a class body in front
of no method of the class's own, answered by its module's open history; a
class's own def that is itself a method body's version behind a late
prepend; a conditional prepend behind another prepend, which the table
takes for a copy that is always there; a `super` that finds no method where
the class defines `method_missing` (the NoMethodError a `super` raises
there is not the call to `method_missing` that CRuby's is), also where no
layer is late.

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

The parents a rebuild climbed each node from are the record's own, and
its readers take them from `lr_par`. A node added since the build, by a
pass such as the desugaring of an included Comparable or by the
registration's walk, takes the tree's parent there, so a climb from any
node the record holds stays within the map.

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

Under `--load-model=inline` required files are spliced into
the one program text, which has one root. The walk is the same over that
text: one region, and a statement a desugaring placed in a body has its
point where it stands, whatever its id.

Under `--load-model=graph`, the default, each file is a root of the node table
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
`PROG_UNKNOWN` against any other region's. A version a region's statement
makes takes the region's extent instead (`lr_version_extent`, read by the
call plan through `elab_version_extent`), so a call of region 0 before the
region's `from` does not meet it and one after its `sure` does. A version
of a class the region makes, or nested in one, keeps its point (no call
reaches the class before the statement that makes it): a class whose every
open is in the region, or one an autoload registers whose file is the
region's (`LoadRecord.rmade`, `lr_region_classes`). That point orders the
version right against a call of another region where the region runs to
its end once entered. It does not tell apart a region that raises before
its later definitions (the raise rescued), one that calls back into code of
another region before them, or an autoload of a constant already defined,
which never loads its file. `--dump-load-graph` prints the
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
