#!/usr/bin/env ruby
# frozen_string_literal: true

# Generate src/builtin_callbacks.inc: for each builtin method of CRuby's core
# (and of the stdlib Spinel implements natively), which methods of the values
# it is handed it calls back, and in what role the value is handed over.
#
#   ruby tools/gen_builtin_callbacks.rb            # print the file
#   ruby tools/gen_builtin_callbacks.rb --write    # regenerate src/builtin_callbacks.inc
#   ruby tools/gen_builtin_callbacks.rb --check    # fail when the file is stale
#                                                  # (make bop-callbacks-check)
#   ruby tools/gen_builtin_callbacks.rb --report   # print the coverage numbers
#
# It is an oracle run, not a proof: CRuby 4.0 runs every builtin with probe
# objects in each position the builtin can take a value, and logs which of the
# probe's methods CRuby calls. The generated file is checked in, so a build
# needs no Ruby (the way src/builtin_share_spec.inc is); --check needs a
# CRuby 4.0 (any patch release) and takes about 25 seconds.
#
# The probe objects. A probe defines a plausible answer for every method name
# a core method might ask of it (CANDIDATES) and logs each call with the role
# of the object it landed on:
#   Full     a program object answering every candidate, so a name that Object
#            has as well (to_s, ==, hash, <=>) shows too. Every Full's `hash`
#            answers 1: two of them in one table collide and `eql?` is asked.
#   Ghost    answers only respond_to_missing? (true) and method_missing, and
#            logs every name CRuby asks or sends: a conversion no list has
#            shows up. The asks are logged under ASKED.
#   Class    a class object whose singleton methods log (raise K, K as a key).
#   Numeric  a program subclass of Numeric (Complex(o) wants a Numeric).
#   Module   a module of the program's, which include and extend call back.
# A hash table compares two keys with eql? when their hashes are equal (and
# may, by chance of the table's hint bits, when they are not), so a role in
# which a builtin calls `hash` is a role in which it calls `eql?`, whether or
# not the probe's keys happened to meet. Evidence, in CRuby 4.0:
#   ruby -e 'class K; def hash = 1; def eql?(o) = (puts "eql?"; false); end; h = {K.new => 1}; h[K.new]'
# prints eql?; the probes' own `hash` answers 1 so that they meet. Which keys
# meet is up to CRuby's hash seed: a table also compares keys whose 8-bit hints
# happen to match, and a Hash key compared with another object asks it for
# to_hash (Hash#eql?, hash_equal in hash.c):
#   ruby -e 'class K; def respond_to_missing?(n, p = false) = (puts n; false); end; {a: 1}.eql?(K.new)'
# prints to_hash. So `hash` brings `eql?`, `to_hash`, and the respond_to? and
# respond_to_missing? that ask for it, all in the role of the `hash`.
#
# Names that dispatch (send, __send__, public_send, method, instance_exec,
# define_method) are rows for what the builtin itself calls back and not for the
# method it is told to call: that method is a call edge, which the call graph's
# rule for a dynamic send owns (a Symbol literal names it; an unknown name is
# the any-method case). So `send` has the conversions of its name argument
# (to_str), and, for a name no method answers, method_missing (respond_to_missing?
# for method and respond_to?); `o.send(:known)` calls nothing back:
#   ruby -e 'class K; def zz = 1; def method_missing(n, *) = puts("mm"); end; K.new.send(:zz)'
# prints nothing. What replaces or ends the process (exec, system, spawn, fork,
# exit!) runs in a forked child that writes each call as it is made, and what
# takes only a block (at_exit) or blocks for ever (Thread.stop) is a row with no
# callback.
#
# The first answer of a protocol hides the alternatives behind it (to_int
# before to_i, marshal_dump before _dump), so a probe that called candidates
# runs again with the names that answered left out, and again with those that
# answered the second time, until nothing new is called (PEEL_ROUNDS).
#
# The roles:
#   ARG    handed as an argument         ELEM   an element of the receiver or
#   BLOCK  answered by a block                  of an argument (a collection)
#   SELF   the receiver, a program       CLASS  a class object handed over
#          subclass of the core class    PORT   an IO argument
#   ASKED  asked through rb_check_funcall: respond_to?, respond_to_missing?,
#          method_missing
#   GVAR   $stdout/$stderr holding the object: assigning it is a write of a port
#          global, which has its own row, so a call keeps nothing for this role
#   LOADED an object the data builds (Marshal.load, YAML.unsafe_load: the class
#          the data names, any class of the program, receives marshal_load,
#          _load or init_with)
#   HOOK   the receiver, a class or module of the program, whose own singleton
#          method the builtin calls: K.const_get(:Z) asks K for const_missing,
#          K.define_method(:m) for method_added
#
# What it writes. Rows are (builtin, receiver, callbacks): the callbacks of
# one builtin on one receiver ("Array", or "File." for a singleton method;
# Kernel's functions are "Kernel"), each with the OR of its roles. A builtin
# probed with no callback is a row with none. The row "*" of a builtin is its
# union over the receivers, the name-level answer for a pass that runs before
# types exist; a core name no receiver was probed for has the row "*" with a
# count of -1. The generated C is included in parts (BCB_PART): 1 the
# callbacks of every row, 2 the row index, 3 the roles by callback name.

require "optparse"
require "set"
require "stringio"
require "timeout"
require "tmpdir"
require "json"
require "pp"
require "yaml"
require "rbconfig"
require "pathname"

ROOT = File.expand_path("..", __dir__)
OUTPUT = File.join(ROOT, "src/builtin_callbacks.inc")
CORE_NAMES = File.join(ROOT, "src/core_method_names.inc")

ENV["TZ"] = "UTC"
Warning[:deprecated] = false
Warning[:experimental] = false
$VERBOSE = nil

ROLES = { arg: 1, elem: 2, block: 4, self: 8, class: 16, port: 32, asked: 64, loaded: 128, gvar: 256, hook: 512 }.freeze
ASKED_NAMES = %w[respond_to? respond_to_missing? method_missing].freeze
# Methods an IO argument is asked for; the same names handed as an ordinary
# argument are the port protocol, not a conversion
PORT_NAMES = %w[write read readpartial flush binmode getbyte].freeze
PEEL_ROUNDS = 4
CALL_BUDGET = 400

# ---- the probe objects ----

LOG = []
BUDGET = [0]
OWN = [nil]   # the method being probed on a program object
class ProbeStop < StandardError; end

FORK_PIPE = [nil]   # inside a forked probe: where each call is written as it is made

def ic_log(name, role)
  LOG << [name.to_s, role]
  FORK_PIPE[0]&.write("#{name}\t#{role}\n")
  BUDGET[0] += 1
  raise ProbeStop, "budget" if BUDGET[0] > CALL_BUDGET
end

# A candidate's answer, called with as many arguments as its body takes
def ic_answer(obj, body, args, kw)
  n = body.arity
  args = n.negative? ? args : (args.first(n) + [nil] * [n - args.size, 0].max)
  obj.instance_exec(*args, **kw, &body)
end

CANDIDATES = {
  marshal_load: ->(_d) { nil },
  to_s: -> { "p" }, inspect: -> { "p" }, to_str: -> { "p" }, name: -> { "P" },
  message: -> { "m" }, full_message: ->(*) { "m" }, detailed_message: ->(*) { "m" },
  to_int: -> { 1 }, to_i: -> { 1 }, hash: -> { 1 }, size: -> { 1 }, length: -> { 1 },
  to_f: -> { 1.0 }, to_r: -> { 1r }, to_c: -> { 1i }, to_sym: -> { :p },
  to_ary: -> { [] }, to_a: -> { [] }, deconstruct: -> { [] }, backtrace: -> { [] },
  to_hash: -> { {} }, to_h: -> { {} }, deconstruct_keys: ->(_k) { {} },
  to_proc: -> { proc { |*| nil } }, to_regexp: -> { /p/ }, to_path: -> { "." },
  to_io: -> { $stdin }, to_open: ->(*) { StringIO.new("") }, to_strio: -> { StringIO.new("") },
  call: ->(*) { nil }, succ: -> { raise ProbeStop, "succ" }, pred: -> { 0 },
  "==": ->(_o) { false }, "eql?": ->(_o) { false }, "===": ->(_o) { false },
  "<=>": ->(o) { equal?(o) ? 0 : -1 }, "=~": ->(_o) { nil }, "!": -> { false }, "!=": ->(_o) { true },
  "+": ->(_o) { 1 }, "-": ->(_o) { 1 }, "*": ->(_o) { 1 }, "/": ->(_o) { 1 },
  "%": ->(_o) { 1 }, "**": ->(_o) { 1 }, "<": ->(_o) { false }, ">": ->(_o) { false },
  "<=": ->(_o) { true }, ">=": ->(_o) { true }, "<<": ->(_o) { 1 }, "&": ->(_o) { 1 },
  "|": ->(_o) { 1 }, "^": ->(_o) { 1 }, "-@": -> { 1 }, "+@": -> { 1 }, "~": -> { 1 },
  "[]": ->(*) { nil }, "[]=": ->(*) { nil },
  coerce: ->(o) { [o, o.is_a?(Numeric) ? o : 1] },
  begin: -> { 0 }, end: -> { 1 }, "exclude_end?": -> { false },
  write: ->(*) { 1 }, read: ->(*) { nil }, readpartial: ->(*) { raise EOFError },
  flush: -> { nil }, binmode: -> { nil }, getbyte: -> { nil },
  "zero?": -> { false }, "nonzero?": -> { 1 }, "positive?": -> { true },
  "negative?": -> { false }, "integer?": -> { true }, "real?": -> { true },
  "finite?": -> { true }, "infinite?": -> { nil },
  abs: -> { 1 }, floor: ->(*) { 1 }, ceil: ->(*) { 1 }, round: ->(*) { 1 }, truncate: ->(*) { 1 },
  div: ->(_o) { 1 }, modulo: ->(_o) { 1 }, divmod: ->(_o) { [1, 0] }, remainder: ->(_o) { 1 },
  fdiv: ->(_o) { 1.0 }, quo: ->(_o) { 1 }, exception: ->(*) { RuntimeError.new("p") },
  cause: -> { nil }, backtrace_locations: -> { nil }, result: -> { nil },
  marshal_dump: -> { 1 }, _dump: ->(_l) { "1" }, encode_with: ->(_c) { nil }, init_with: ->(_c) { nil },
  default: ->(*) { nil }, to_json: ->(*) { "1" }, as_json: ->(*) { 1 }, dig: ->(*) { nil },
  utc_to_local: ->(t) { t }, instance_variables_to_inspect: -> { [] },
  pretty_print: ->(q) { q.pp(self) }, pretty_print_cycle: ->(q) { q.text("p") },   # pp(self) again: the cycle
}.freeze
CANDIDATE_NAMES = CANDIDATES.keys.map(&:to_s).freeze
# Names a probe cannot leave out: the probe's own machinery, and `hash`, which
# stays 1 so that two probes in a table collide (a real hash code would make
# `eql?` depend on where the objects happen to live)
STRUCTURAL = %w[respond_to? respond_to_missing? method_missing initialize_copy initialize_dup
                initialize_clone each each_pair hash].freeze
# What a class object answers by its candidate body and not by Module's: `hash`
# (two probes in a table collide), and `<=>`, which Module answers nil for two
# unrelated classes, so a Range of two class objects could not be made and
# `(A..B).each` never reached `A.succ`
CLASS_OWN_ANSWER = %i[hash <=>].freeze
CLASS_ONLY = {   # singleton methods only a class object has
  _load: ->(_s) { Object.new }, const_missing: ->(_n) { raise NameError }, inherited: ->(_k) { nil },
  # the hooks Module runs on a class as the program defines and removes its members
  **%i[method_added method_removed method_undefined singleton_method_added singleton_method_removed
       singleton_method_undefined const_added].to_h { |n| [n, ->(_n) { nil }] },
}.freeze

module Probe
  @serial = 0
  @pool = {}

  # A named class (Marshal dumps by class name), built once per configuration
  def self.pooled(key)
    @pool[key] ||= yield.tap { |k| Object.const_set("ICProbe#{@serial += 1}", k) }
  end

  def self.define_asked(k, role)
    k.define_method(:respond_to?) { |n, priv = false| ic_log(:respond_to?, :asked); super(n, priv) }
    k.define_method(:respond_to_missing?) { |n, priv = false| ic_log(:respond_to_missing?, :asked); super(n, priv) }
    k.define_method(:method_missing) do |n, *a, &b|
      ic_log(:method_missing, :asked) unless n.to_s == OWN[0]   # the probed method itself, undefined here
      super(n, *a, &b)
    end
    %i[initialize_copy initialize_dup initialize_clone].each do |n|
      k.define_method(n) { |*a, **kw, &b| ic_log(n, role); super(*a, **kw, &b) }
    end
  end

  # The program subclass (or includer) of `base` whose instances log: every
  # candidate but those in `omit`. With `bodies` a candidate answers by its
  # body; without, it calls the base's method when there is one, so the object
  # stays a working Hash or Exception.
  def self.instance_class(base, role, omit, bodies)
    pooled([:inst, base, role, omit, bodies]) do
      k = base.is_a?(Class) ? Class.new(base) : Class.new { include base }
      k.include(Comparable, Enumerable) if base == Object
      CANDIDATES.each do |n, body|
        next if omit.include?(n.to_s)
        if bodies
          k.define_method(n) { |*a, **kw, &b| ic_log(n, role); ic_answer(self, body, a, kw) }
        else
          k.define_method(n) do |*a, **kw, &b|
            ic_log(n, role)
            defined?(super) ? super(*a, **kw, &b) : ic_answer(self, body, a, kw)
          end
        end
      end
      %i[default result].each do |n|   # the base's own hooks nothing else asks for
        next unless !bodies && (base.method_defined?(n) || base.private_method_defined?(n))
        k.define_method(n) { |*a, **kw, &b| ic_log(n, role); super(*a, **kw, &b) }
      end
      if bodies
        k.define_method(:each) { |&b| ic_log(:each, role); b&.call(1); self }
        k.define_method(:each_pair) { |&b| ic_log(:each_pair, role); b&.call(1, 2); self }
      end
      # the loader of a class whose objects are dumped by `_dump`
      k.define_singleton_method(:_load) { |*| ic_log(:_load, role); new } unless omit.include?("_load")
      define_asked(k, role)
      k
    end
  end

  def self.ghost_class(role)
    pooled([:ghost, role]) do
      Class.new do
        define_method(:respond_to_missing?) { |n, _priv = false| ic_log(n, role); ic_log(:respond_to_missing?, :asked); true }
        define_method(:method_missing) { |n, *_a, &_b| ic_log(n, role); ic_log(:method_missing, :asked); nil }
      end
    end
  end

  # A class object whose singleton methods log; `idx` picks one of several
  # distinct classes (a pair of keys)
  def self.class_object(omit, idx, role = :class)
    pooled([:klass, omit, idx, role]) do
      k = Class.new(StandardError)
      CANDIDATES.each do |n, body|
        next if omit.include?(n.to_s)
        k.define_singleton_method(n) do |*a, **kw, &b|
          ic_log(n, role)
          defined?(super) && !CLASS_OWN_ANSWER.include?(n) ? super(*a, **kw, &b) : ic_answer(self, body, a, kw)
        end
      end
      CLASS_ONLY.each do |n, body|
        next if omit.include?(n.to_s)
        k.define_singleton_method(n) { |*a, **kw, &b| ic_log(n, role); ic_answer(self, body, a, kw) }
      end
      k.define_singleton_method(:new) { |*a, &b| ic_log(:new, role); super(*a, &b) } unless omit.include?("new")
      k.define_singleton_method(:respond_to?) { |n, priv = false| ic_log(:respond_to?, :asked); super(n, priv) }
      k.define_singleton_method(:respond_to_missing?) { |n, priv = false| ic_log(:respond_to_missing?, :asked); super(n, priv) }
      k
    end
  end

  # A module of the program's, whose singleton methods log: what include and
  # extend call back on it (`append_features`, `included`, `extend_object`,
  # `extended`, `prepend_features`, `prepended`) beside the candidates
  MODULE_HOOKS = %i[append_features included extend_object extended prepend_features prepended].freeze
  def self.module_object(role, omit, idx)
    pooled([:module, role, omit, idx]) do
      k = Module.new
      CANDIDATES.each do |n, body|
        next if omit.include?(n.to_s)
        k.define_singleton_method(n) do |*a, **kw, &b|
          ic_log(n, role)
          defined?(super) && n != :hash ? super(*a, **kw, &b) : ic_answer(self, body, a, kw)
        end
      end
      MODULE_HOOKS.each { |n| k.define_singleton_method(n) { |*a| ic_log(n, role); super(*a) } }
      k.define_singleton_method(:respond_to?) { |n, priv = false| ic_log(:respond_to?, :asked); super(n, priv) }
      k.define_singleton_method(:respond_to_missing?) { |n, priv = false| ic_log(:respond_to_missing?, :asked); super(n, priv) }
      k
    end
  end

  # A program subclass of a core class, made the way the core class is
  def self.sample_of(base, klass)
    case base.name
    when "Numeric" then klass.new
    when "Hash" then klass.new.tap { |h| h[1] = 2 }
    when "Array" then klass.new([1, 2])
    when "String" then klass.new("ab")
    when "Range" then klass.new(1, 3)
    when "Set" then klass.new([1, 2])
    when "StringIO" then klass.new(+"ab")
    when "Time" then klass.at(0)
    when "Proc" then klass.new { |*a| a }
    else
      return klass.new("m") if base <= Exception
      base.is_a?(Class) ? klass.new(1) : klass.new
    end
  end

  # The object of one kind for one role. A SELF receiver, or an object of a
  # `base` the caller names, is a program subclass whatever the kind.
  def self.make(kind, role, omit, idx, base)
    return class_object(omit, idx, :hook) if role == :hook   # the receiver, whatever the arguments are
    if role == :self || base
      bodies = base.nil? || base == Numeric || !base.is_a?(Class)
      return sample_of(base || Object, instance_class(base || Object, role, omit, bodies)) if base
      return instance_class(Object, role, omit, true).new
    end
    case kind
    when :full then instance_class(Object, role, omit, true).new
    when :numeric then instance_class(Numeric, role, omit, true).new
    when :ghost then ghost_class(role).new
    when :klass then class_object(omit, idx, role == :gvar ? :gvar : :class)
    when :module then module_object(role, omit, idx)
    end
  end
end

# Hands out fresh probe objects of one kind; `omit` maps a role to the
# candidate names its objects leave out
class Maker
  NONE = [].freeze

  def initialize(kind, omit)
    @kind = kind
    @omit = omit
    @n = 0
  end

  # `without`: candidates this object leaves out besides the peeled ones
  def call(role, base: nil, without: NONE)
    omit = @omit.fetch(role, NONE) | without
    # a class object is logged as a :class however it is handed over, so the
    # names its calls peeled are under that role
    omit |= @omit.fetch(:class, NONE) if @kind == :klass
    omit = omit.sort.freeze
    Probe.make(@kind, role, omit, (@n += 1) - 1, base)
  end
end

# ---- running a probe ----

ROWS = Hash.new { |h, k| h[k] = Set.new }   # [receiver, builtin] => [role bit, callback]
STATS = Hash.new(0)
TIMED_OUT = []   # the probes that ran into the time limit, for --report
KINDS = %i[full ghost klass numeric module].freeze

def effective_role(name, role)
  return :asked if ASKED_NAMES.include?(name)
  return :port if role == :arg && PORT_NAMES.include?(name)
  role
end

# One run: build the call (not logged), clear the log, make the call. nil
# when the call could not be built.
def run_once(kind, omit)
  LOG.clear
  thunk = begin
    yield Maker.new(kind, omit)
  rescue StandardError, NotImplementedError, ScriptError, ProbeStop
    STATS[:build_failed] += 1
    return nil
  end
  LOG.clear
  BUDGET[0] = 0
  out = $stdout
  err = $stderr
  inp = $stdin
  $stdout = StringIO.new
  $stderr = StringIO.new
  $stdin = StringIO.new
  begin
    Timeout.timeout(0.5) { thunk.call }
    STATS[:ok] += 1
  rescue Timeout::Error
    STATS[:timeout] += 1
  rescue ProbeStop, StandardError, NotImplementedError, ScriptError, SystemExit
    STATS[:raised] += 1
  rescue SystemStackError
    STATS[:stack] += 1
  ensure
    $stdout = out
    $stderr = err
    $stdin = inp
  end
  STATS[:runs] += 1
  GC.start if (STATS[:runs] % 2000).zero?   # the files and threads of receivers made on the way
  LOG.dup
end

# Probe one call in every kind of object, peeling the names that answer: each
# round leaves out, per role, the candidates the last one called. `own` is the
# method being probed on a program object (its own name is no callback of
# itself, as the role `own_role` logs it); `self_omit` leaves it out of the receiver.
def probe(recv, builtin, own: nil, self_omit: [], own_role: :self, &build)
  key = [recv, builtin]
  OWN[0] = own
  KINDS.each do |kind|
    left = self_omit.empty? ? {} : { own_role => self_omit.sort.freeze }
    (kind == :ghost ? 1 : PEEL_ROUNDS).times do
      timeouts = STATS[:timeout]
      events = run_once(kind, left, &build)
      TIMED_OUT << key if STATS[:timeout] > timeouts
      break unless events
      events.reject! { |n, role| role == own_role && n == own } if own
      events.each { |n, role| ROWS[key] << [ROLES.fetch(effective_role(n, role)), n] }
      ROWS[key]
      grew = false
      events.group_by(&:last).each do |role, es|
        fresh = (es.map(&:first).uniq & CANDIDATE_NAMES) - left.fetch(role, []) - STRUCTURAL
        next if fresh.empty?
        left[role] = (left.fetch(role, []) | fresh).sort.freeze
        grew = true
      end
      break unless grew
    end
  end
  # a hash table compares keys with eql?, and a Hash key asked to_hash by it:
  # whatever role is called for `hash` is called for these (see the head of the file)
  ROWS[key].select { |_, cb| cb == "hash" }.each do |bit, _|
    ROWS[key] << [bit, "eql?"] << [bit, "to_hash"] << [ROLES[:asked], "respond_to?"] << [ROLES[:asked], "respond_to_missing?"]
  end
  STATS[:probes] += 1
end

# ---- the positions of a call ----

def arity_counts(meth)
  a = meth.arity
  req = a >= 0 ? a : -a - 1
  max = a >= 0 ? a : [req + 2, 2].max
  (req..[max, 2].min).to_a
end

# The argument lists tried for n arguments, as [argument maker, block maker]:
# a probe in one slot (the rest 1 or "a"), in every slot, two probes in an
# Array or a Hash (ELEM); a block that answers a probe (BLOCK) or the first
# thing it is yielded (an element of the receiver).
def argument_shapes(n, elems, fills = [1, "a"], plain = [1])
  shapes = plain.map { |fill| [->(_mk) { Array.new(n, fill) }, ->(mk) { mk.(:block) }] }
  shapes.concat(plain.map { |fill| [->(_mk) { Array.new(n, fill) }, ->(_mk) { proc { |*a| a.first } }] }) if elems
  return shapes unless n.positive?
  shapes << [->(mk) { Array.new(n) { mk.(:arg) } }, nil]
  n.times do |i|
    fills.each do |fill|
      shapes << [->(mk) { Array.new(n, fill).tap { |a| a[i] = mk.(:arg) } }, nil]
    end
    shapes << [->(mk) { Array.new(n, 1).tap { |a| a[i] = [mk.(:elem), mk.(:elem)] } }, nil]
    shapes << [->(mk) { Array.new(n, 1).tap { |a| a[i] = { mk.(:elem) => mk.(:elem) } } }, nil]
  end
  shapes
end

# Every call of `mname` on the receiver `build_recv.(mk)` makes
def probe_calls(label, mname, counts, build_recv, own: nil, elems: false, own_role: :self, fills: [1, "a"], plain: [1], like: nil)
  counts.each do |n|
    shapes = argument_shapes(n, elems, fills, plain)
    # a container of the receiver's own shape as the one argument: two
    # containers compare element by element (Hash#== asks a value's ==)
    shapes << [->(mk) { [like.(mk)] }, nil] if like && n == 1 && %w[== != eql? ===].include?(mname.to_s)
    shapes.each do |args, blk|
      probe(label, mname.to_s, own: own, self_omit: own ? [own] : [], own_role: own_role) do |mk|
        recv = build_recv.(mk)
        a = args.(mk)
        b = blk&.(mk)
        b = proc { |*| b } if b && !b.is_a?(Proc)
        -> { recv.__send__(mname, *a, &b) }
      end
    end
  end
end

# Names that stop the run or change the process, and so are not run in this
# process whatever the receiver: they end it, fork it, hand it to a command,
# sleep, or stop the interpreter. Kernel's own functions that are not run
# (select, loop, open, raise ...) are KERNEL_SKIP's; a method of another
# receiver with such a name (Array#select, IO#puts, Thread#raise,
# Enumerable#cycle) is probed like any other.
SKIP_ANYWHERE = %i[
  exit! fork exec system spawn sleep trap syscall ` set_trace_func trace_var callcc irb verify_internal_consistency
  at_exit loop catch
].to_set.freeze
# On a program object dup and clone run initialize_copy, and the output goes
# to the captured $stdout: they are probed
SELF_SKIP = SKIP_ANYWHERE
# Singleton methods that are not run in this process: they end it or change its
# cwd or signals, take a pid, or are probed in a forked child. The file system
# ones run, in a directory of their own.
SINGLETON_SKIP = %i[allocate exit! fork _fork exec spawn kill daemon
  setsid setpgid setpgrp setrlimit setproctitle setpriority setuid setgid seteuid setegid setreuid setregid
  setresuid setresgid initgroups warmup chdir chroot popen sysopen for_fd mktmpdir tmpdir stop set_scheduler
  garbage_collect verify_compaction_references each_object define_finalizer _id2ref
  create_id= state= generator= parser= dump_default_options= load_default_options=].to_set.freeze
# Builtins that are not run (they register a block for the end of the program,
# stop the thread for ever, or daemonize the process) and are handed nothing a
# probe could be: at_exit's block, Thread.stop and Process.daemon take no value
NO_OBJECT = [["Kernel", "at_exit"], ["Thread.", "stop"], ["Process.", "daemon"]].freeze
# `new` of the classes whose constructor takes values; File.new(1) would wrap fd 1
NEW_OK = %w[Array Hash String Set StringIO Range Regexp Time Class Module Thread Fiber Random].freeze
KERNEL_SKIP = %i[exit! fork exec system spawn sleep trap syscall ` select loop open
  set_trace_func trace_var at_exit callcc catch raise].to_set.freeze
# Per receiver: instance methods that block (a Queue's pop), singleton methods that end the thread
INSTANCE_SKIP_OF = { "Queue" => %i[pop shift deq],
                     # the file system, and a Ractor's blocking calls
                     "Pathname" => %i[rmtree rmdir unlink delete rename mkdir mkpath truncate chmod lchmod chown lchown utime
                                      make_symlink make_link write binwrite lutime],
                     "Ractor" => %i[take join value receive recv close_incoming close_outgoing monitor unmonitor] }.freeze
# receivers probed by their own methods alone
OWN_METHODS_ONLY = %w[Ractor Refinement].freeze
SINGLETON_SKIP_OF = { "Thread" => %i[exit kill stop] }.freeze

INSTANCE_RECEIVERS = {
  "Integer" => -> { 3 }, "Float" => -> { 2.5 }, "String" => -> { +"ab" }, "Symbol" => -> { :ab },
  "Array" => -> { [1, 2] }, "Hash" => -> { { 1 => 2 } }, "Range" => -> { (1..2) },
  "Rational" => -> { 1r }, "Complex" => -> { 1i }, "Regexp" => -> { /a/ }, "Time" => -> { Time.at(0) },
  "Set" => -> { Set[1, 2] }, "NilClass" => -> { nil }, "StringIO" => -> { StringIO.new(+"ab") },
  "Struct" => -> { Struct.new(:a).new(1) }, "Proc" => -> { proc { |*a| a } },
  "Method" => -> { 1.method(:+) }, "Enumerator" => -> { [1, 2].each },
  "UnboundMethod" => -> { Integer.instance_method(:+) }, "Binding" => -> { binding },
  # Module and Class: the probes are the arguments (include, extend, define_method)
  "Module" => -> { Module.new }, "Class" => -> { Class.new },
  # a thread that has run, a fiber, a file on /dev/null, a directory, a match
  "Thread" => -> { Thread.new { 1 }.tap(&:join) }, "Fiber" => -> { Fiber.new { 1 } },
  "File" => -> { File.new(File::NULL) }, "Dir" => -> { Dir.new(".") }, "Encoding" => -> { Encoding::UTF_8 },
  "MatchData" => -> { /(?<x>a)b/.match("ab") }, "Random" => -> { Random.new(1) },
  "Data" => -> { Data.define(:a).new(a: 1) }, "Process::Status" => -> { PSTATUS },
  "Queue" => -> { Thread::Queue.new }, "Mutex" => -> { Thread::Mutex.new },
  "TracePoint" => -> { TracePoint.new(:call) { } },
  # Pathname (core in CRuby 4.0), and the two core classes Spinel has no
  # counterpart for: their methods are probed so that no core name is left
  # without a row (their own methods only: the rest are Object's and Module's)
  "Pathname" => -> { Pathname.new(File::NULL) }, "Ractor" => -> { Ractor.current },
  "Refinement" => -> { Module.new { refine(String) { } }.refinements.first },
  # the exceptions that carry a value of their own
  "NoMethodError" => -> { NoMethodError.new("m", :n, [1], receiver: 1) }, "NameError" => -> { NameError.new("m", :n, receiver: 1) },
  "KeyError" => -> { KeyError.new("m", receiver: {}, key: 1) }, "FrozenError" => -> { FrozenError.new("m", receiver: 1) },
  "SystemCallError" => -> { Errno::ENOENT.new }, "SignalException" => -> { SignalException.new("INT") },
  "UncaughtThrowError" => -> { UncaughtThrowError.new(:t, 1, "m") }, "LocalJumpError" => -> { LocalJumpError.new("m") },
}.freeze
PSTATUS = (system("true"); $?)
# Containers of probe objects: what a builtin calls on its elements. Two
# elements, so that their equal hashes collide and `eql?` is asked.
CONTAINERS = {
  "Array" => ->(mk) { [mk.(:elem), mk.(:elem)] },
  # pairs, which assoc and rassoc compare by their first or second element
  "Array pairs" => ->(mk) { [[mk.(:elem), mk.(:elem)], [mk.(:elem), mk.(:elem)]] },
  "Hash" => ->(mk) { { mk.(:elem) => 1, mk.(:elem) => 2 } },
  "Hash values" => ->(mk) { { 1 => mk.(:elem), 2 => mk.(:elem) } },
  "Set" => ->(mk) { Set[mk.(:elem), mk.(:elem)] },
  "Struct" => ->(mk) { Struct.new(:a, :b).new(mk.(:elem), mk.(:elem)) },
  "Range" => ->(mk) { Range.new(mk.(:elem), mk.(:elem)) },
  "Range one end" => ->(mk) { o = mk.(:elem); Range.new(o, o) },
}.freeze
SINGLETON_RECEIVERS = {
  "Kernel" => Kernel, "Math" => Math, "File" => File, "Dir" => Dir, "IO" => IO, "Marshal" => Marshal,
  "Array" => Array, "Hash" => Hash, "String" => String, "Integer" => Integer, "Float" => Float,
  "Rational" => Rational, "Complex" => Complex, "Regexp" => Regexp, "Time" => Time, "Range" => Range,
  "Struct" => Struct, "Comparable" => Comparable, "ObjectSpace" => ObjectSpace, "Symbol" => Symbol,
  "Set" => Set, "StringIO" => StringIO, "PP" => PP, "JSON" => JSON, "YAML" => YAML,
  "Thread" => Thread, "Fiber" => Fiber, "Process" => Process, "Signal" => Signal, "Encoding" => Encoding,
  "Random" => Random, "Module" => Module, "Class" => Class, "GC" => GC, "Data" => Data,
}.freeze
# Program subclasses of core classes (and includers of core modules): what a
# core method calls on self
SUBCLASS_OF = [Numeric, Hash, Array, String, Exception, StopIteration, Range, Set,
               Struct.new(:a), Comparable, Enumerable, StringIO, Time, Proc].freeze

def probe_instances
  INSTANCE_RECEIVERS.each do |cname, mk|
    names = OWN_METHODS_ONLY.include?(cname) ? mk.call.class.public_instance_methods(false) : mk.call.public_methods
    names.sort.each do |mname|
      next if SKIP_ANYWHERE.include?(mname) || INSTANCE_SKIP_OF.fetch(cname, []).include?(mname) || (mname.end_with?("!") && cname == "NilClass")
      m = (mk.call.method(mname) rescue next)
      probe_calls(cname, mname, arity_counts(m), ->(_mk) { mk.call })
    end
  end
  CONTAINERS.each do |cname, mkc|
    probe_one = Maker.new(:full, {})
    mkc.(probe_one).public_methods.sort.each do |mname|
      next if SKIP_ANYWHERE.include?(mname)
      m = (mkc.(probe_one).method(mname) rescue next)
      probe_calls(cname.split.first, mname, arity_counts(m), mkc, elems: true, like: mkc)
    end
  end
end

def probe_subclasses
  SUBCLASS_OF.each do |base|
    label = base.name || "Struct"
    probe_obj = (Probe.make(:full, :self, [].freeze, 0, base) rescue nil)
    next unless probe_obj
    names = base.is_a?(Class) ? base.public_instance_methods : base.public_instance_methods(false)
    names.sort.each do |mname|
      next if SELF_SKIP.include?(mname)
      m = (probe_obj.method(mname) rescue next)
      probe_calls(label, mname, arity_counts(m), ->(mk) { mk.(:self, base: base) }, own: mname.to_s, elems: true)
    end
  end
  # Kernel's and BasicObject's own methods on a program object
  { "Kernel" => Kernel, "BasicObject" => BasicObject }.each do |label, owner|
    owner.public_instance_methods(false).sort.each do |mname|
      next if SELF_SKIP.include?(mname)
      probe_calls(label, mname, arity_counts(owner.instance_method(mname)), ->(mk) { mk.(:self) }, own: mname.to_s)
    end
  end
end

# Module's and Class's methods on a class of the program that logs its own
# singleton hooks. A name argument is also tried as a constant's (Z), which
# const_get wants before it asks for const_missing, and a method or constant
# name is tried with the block define_method wants.
def probe_hooks
  owners = { Module => "Module", Class => "Class" }
  Class.public_instance_methods.sort.each do |mname|
    m = Class.instance_method(mname)
    next if !owners.key?(m.owner) || SKIP_ANYWHERE.include?(mname)
    probe_calls(owners[m.owner], mname, arity_counts(m), ->(mk) { mk.(:hook) },
                own: mname.to_s, own_role: :hook, fills: [1, "a", "Z"], plain: [1, "Z"])
  end
end

def probe_singletons
  SINGLETON_RECEIVERS.each do |cname, mod|
    names = mod.singleton_methods(true) | (mod == Kernel ? Kernel.private_instance_methods(false) : [])
    names.sort.each do |mname|
      next if SINGLETON_SKIP.include?(mname) || SINGLETON_SKIP_OF.fetch(cname, []).include?(mname) || (SKIP_ANYWHERE.include?(mname) && mod != Kernel)
      next if mod == Kernel && KERNEL_SKIP.include?(mname)
      next if mname == :new && !NEW_OK.include?(cname)
      next if mname.end_with?("=") && !%i[== != <= >= ===].include?(mname)
      m = (mod.method(mname) rescue next)
      probe_calls(mod == Kernel ? "Kernel" : "#{cname}.", mname, arity_counts(m), ->(_mk) { mod })
    end
  end
end

# ---- calls the loops above do not make ----

# A builtin that replaces or ends the process runs in a forked child, which
# writes each call as it is made. The probes are as in probe_calls.
FORKED = { "Kernel" => %i[exit! fork exec system spawn `], "Process." => %i[exit! exec spawn], "IO." => %i[popen for_fd sysopen],
           "Signal." => %i[trap] }.freeze

def probe_forked(label, mname, mod)
  m = (mod.method(mname) rescue return)
  arity_counts(m).each do |n|
    argument_shapes(n, false).each do |args, blk|
      KINDS.each do |kind|
        r, w = IO.pipe
        pid = fork do
          r.close
          w.sync = true
          begin
            mk = Maker.new(kind, {})
            a = args.(mk)
            b = blk&.(mk)
            b = proc { |*| b } if b && !b.is_a?(Proc)
            LOG.clear
            FORK_PIPE[0] = w
            $stdout = $stderr = File.open(File::NULL, "w")
            Timeout.timeout(0.5) { mod.__send__(mname, *a, &b) }
          rescue Exception # rubocop:disable Lint/RescueException
            nil
          end
          exit!(0)
        end
        w.close
        events = r.read.lines.map { |l| l.chomp.split("\t") }.select { |e| e.size == 2 }
        r.close
        Process.wait(pid)
        key = [label, mname.to_s]
        events.each { |nm, role| ROWS[key] << [ROLES.fetch(effective_role(nm, role.to_sym)), nm] }
        ROWS[key]
      end
    end
  end
  STATS[:probes] += 1
end

def probe_forked_all
  FORKED.each { |label, names| names.each { |n| probe_forked(label, n, label == "Kernel" ? Kernel : Object.const_get(label.delete_suffix("."))) } }
end

def with_gvar(name, value)
  prev = name == "$stderr" ? $stderr : $stdout
  name == "$stderr" ? $stderr = value : $stdout = value
  yield
ensure
  name == "$stderr" ? $stderr = prev : $stdout = prev
end

# An output builtin prints what it is handed (ARG, ELEM); with $stdout or
# $stderr holding a program object it asks that object for `write` (PORT).
def probe_output
  %i[puts print p pp printf putc warn].each do |mname|
    fmt = mname == :printf ? ["%s"] : []
    gvar = mname == :warn ? "$stderr" : "$stdout"
    probe("Kernel", mname.to_s) { |mk| o = mk.(:arg); -> { Kernel.__send__(mname, *fmt, o) } }
    probe("Kernel", mname.to_s) { |mk| a = [mk.(:elem), mk.(:elem)]; -> { Kernel.__send__(mname, *fmt, a) } }
    probe("Kernel", mname.to_s) { |mk| port = mk.(:gvar); -> { with_gvar(gvar, port) { Kernel.__send__(mname, *fmt, "x") } } }
  end
  probe("Kernel", "printf") { |mk| io = mk.(:port); -> { Kernel.printf(io, "x") } }
  probe("Kernel", "display") { |mk| io = mk.(:port); -> { "x".display(io) } }
  probe("Kernel", "display") { |mk| io = mk.(:gvar); -> { with_gvar("$stdout", io) { "x".display } } }
end

# Calls whose position is not a plain argument, or that need a set-up
def probe_kernel
  probe("Kernel", "raise") { |mk| o = mk.(:arg); -> { raise o } }
  probe("Kernel", "raise") { |mk| o = mk.(:arg, base: StandardError); -> { raise o } }
  probe("Kernel", "raise") { |mk| o = mk.(:arg); -> { raise RuntimeError, o } }
  probe("Kernel", "raise") { |mk| o = mk.(:arg); -> { raise o, "x" } }
  probe("Kernel", "loop") { |mk| o = mk.(:self, base: StopIteration); -> { loop { raise o } } }
  probe("Kernel", "catch") { |mk| o = mk.(:arg); -> { catch(o) { throw o } } }
  probe("Kernel", "sleep") { |mk| o = mk.(:arg); -> { sleep(o) } }
  probe("Kernel", "open") { |mk| o = mk.(:arg); -> { open(o) { |f| f } } }
  probe("Kernel", "format") { |mk| o = mk.(:arg); -> { format("%s %d %f", o, o, o) } }
  probe("Kernel", "sprintf") { |mk| o = mk.(:arg); -> { sprintf("%s %d %f", o, o, o) } }
  # a Hash argument holds the values a named reference (%{a}, %<b>s) reads:
  # they are its elements, not the argument
  %i[format sprintf printf].each do |mname|
    probe("Kernel", mname.to_s) { |mk| h = { a: mk.(:elem), b: mk.(:elem) }; -> { Kernel.__send__(mname, "%{a} %<b>s", h) } }
  end
  probe("File", "printf") { |mk| h = { a: mk.(:elem), b: mk.(:elem) }; f = File.new(File::NULL, "w"); -> { f.printf("%{a} %<b>s", h) } }
  probe("StringIO", "printf") { |mk| h = { a: mk.(:elem), b: mk.(:elem) }; io = StringIO.new(+""); -> { io.printf("%{a} %<b>s", h) } }
  probe("String", "%") { |mk| o = mk.(:arg); -> { +"%s %d %f" % [o, o, o] } }
  # the Array or Hash a program writes as the argument holds the values the
  # format reads: they are its elements, not the argument
  probe("String", "%") { |mk| a = [mk.(:elem), mk.(:elem)]; -> { +"%s %d" % a } }
  probe("String", "%") { |mk| h = { a: mk.(:elem), b: mk.(:elem) }; -> { +"%{a} %<b>s" % h } }
  # a String argument joins the elements: to_str, to_ary, then to_s
  probe("Array", "*") { |mk| a = [mk.(:elem), mk.(:elem)]; -> { a * "," } }
  probe("Array", "pack") { |mk| a = [mk.(:elem), mk.(:elem)]; -> { a.pack("aw") } }
  probe("IO.", "select") { |mk| o = mk.(:elem); -> { IO.select([o], nil, nil, 0) } }
  probe("Kernel", "select") { |mk| o = mk.(:elem); -> { select([o], nil, nil, 0) } }
  probe("Marshal.", "load") { |mk| data = Marshal.dump(mk.(:loaded)); -> { Marshal.load(data) } }
  # an object dumped by `_dump`, which a class method `_load` loads
  probe("Marshal.", "load") { |mk| data = Marshal.dump(mk.(:loaded, without: %w[marshal_dump])); -> { Marshal.load(data) } }
  probe("Marshal.", "dump") { |mk| o = mk.(:arg); io = mk.(:port); -> { Marshal.dump(o, io) } }
  probe("Marshal.", "load") { |mk| io = mk.(:port); -> { Marshal.load(io) } }
  probe("IO.", "copy_stream") { |mk| io = mk.(:port); -> { IO.copy_stream(io, StringIO.new) } }
  probe("IO.", "copy_stream") { |mk| io = mk.(:port); -> { IO.copy_stream(StringIO.new("ab"), io) } }
  probe("PP.", "pp") { |mk| e = mk.(:elem); a = [e]; a << a; -> { PP.pp(a, +"") } }
  probe("YAML.", "unsafe_load") { |mk| data = YAML.dump(mk.(:loaded)); -> { YAML.unsafe_load(data) } }
  # cycle(1) ends; a bare cycle never does
  probe("Array", "cycle") { |mk| a = [mk.(:elem), mk.(:elem)]; -> { a.cycle(1) { |x| x } } }
  probe("Enumerable", "cycle") { |mk| o = mk.(:self); -> { o.cycle(1) { |x| x } } }
  # builtins that take a block or nothing, and so are handed no object now
  NO_OBJECT.each { |recv, name| probe(recv, name) { |_mk| -> { 1 } } }
  probe("ObjectSpace.", "define_finalizer") do |mk|
    o = mk.(:arg)
    target = Object.new
    lambda do
      ObjectSpace.define_finalizer(target, o)
    ensure
      ObjectSpace.undefine_finalizer(target)
    end
  end
end

# ---- the table ----

def universe_names
  names = File.read(CORE_NAMES).scan(/^\s*"((?:[^"\\]|\\.)*)",/).flatten.map { |s| JSON.parse("\"#{s}\"") }
  code = <<~'RUBY'
    core = ObjectSpace.each_object(Module).select { |m| (n = m.name) && !n.include?("::") && Object.const_get(n).equal?(m) rescue false }
    puts core.flat_map { |m| m.singleton_methods(false) }.map(&:to_s).uniq.sort
  RUBY
  singles = IO.popen([RbConfig.ruby, "--disable-gems", "-e", code], &:read).split("\n")
  [names, singles]
end

# [[builtin, receiver, [[callback, roles], ...]], ...] with the "*" rows
def build_table
  by_builtin = Hash.new { |h, k| h[k] = {} }
  ROWS.each do |(recv, builtin), events|
    calls = events.group_by(&:last).transform_values { |es| es.map(&:first).reduce(0, :|) }
    by_builtin[builtin][recv] = calls.sort.to_a
  end
  core, singles = universe_names
  (core | singles).each { |n| by_builtin[n]["*"] = nil unless by_builtin.key?(n) }
  rows = []
  by_builtin.sort.each do |builtin, recvs|
    if recvs.key?("*")
      rows << [builtin, "*", nil]
      next
    end
    union = Hash.new(0)
    recvs.each_value { |calls| calls.each { |cb, roles| union[cb] |= roles } }
    rows << [builtin, "*", union.sort.to_a]
    recvs.sort.each { |recv, calls| rows << [builtin, recv, calls] }
  end
  rows
end

def role_text(mask)
  ROLES.select { |_, bit| mask & bit != 0 }.keys.map { |r| "BCR_#{r.upcase}" }.join(" | ")
end

def generate(rows)
  pairs = []
  runs = {}    # a row's callbacks => where they are in pairs: identical runs are emitted once
  index = []
  rows.each do |builtin, recv, calls|
    first = calls && !calls.empty? ? (runs[calls] ||= (pairs.size.tap { pairs.concat(calls) })) : 0
    index << [builtin, recv, first, calls ? calls.size : -1]
  end
  by_callback = Hash.new(0)
  pairs.each { |cb, roles| by_callback[cb] |= roles }
  out = +"/* Generated by tools/gen_builtin_callbacks.rb from a CRuby 4.0 run: do not edit.\n"
  out << "   What each builtin calls back on the values it is handed (see the head of\n"
  out << "   the generator). Included three times, with BCB_PART set to:\n"
  out << "     1  the runs of callbacks, { name, roles }, each run sorted by name and\n"
  out << "        emitted once however many rows have it\n"
  out << "     2  the rows, { builtin, receiver, first callback of its run, count }, sorted\n"
  out << "        by builtin and receiver; the receiver \"*\" is the union over receivers,\n"
  out << "        count -1 a core name no receiver was probed for\n"
  out << "     3  the roles by callback name, { name, roles }, sorted */\n"
  out << "#if BCB_PART == 1\n"
  pairs.each { |cb, roles| out << "  { #{cb.dump}, #{role_text(roles)} },\n" }
  out << "#elif BCB_PART == 2\n"
  index.each { |b, r, first, n| out << "  { #{b.dump}, #{r.dump}, #{first}, #{n} },\n" }
  out << "#elif BCB_PART == 3\n"
  by_callback.sort.each { |cb, roles| out << "  { #{cb.dump}, #{role_text(roles)} },\n" }
  out << "#endif\n"
  out
end

def report(rows)
  core, singles = universe_names
  probed = rows.reject { |_, r, c| r == "*" || c.nil? }
  names = rows.select { |_, r, _| r == "*" }
  with_cb = names.select { |_, _, c| c && !c.empty? }
  cbs = names.flat_map { |_, _, c| c || [] }.map(&:first).uniq
  pairs = names.sum { |_, _, c| c ? c.size : 0 }
  puts "probes #{STATS[:probes]}, runs #{STATS[:runs]} (ok #{STATS[:ok]}, raised #{STATS[:raised]}, " \
       "timeout #{STATS[:timeout]}, stack #{STATS[:stack]}), builds failed #{STATS[:build_failed]}"
  puts "rows #{probed.size} (builtin, receiver); builtin names #{names.size - names.count { |_, _, c| c.nil? }} probed, " \
       "#{names.count { |_, _, c| c.nil? }} uncovered"
  puts "builtin names with a callback #{with_cb.size}; (name, callback) pairs #{pairs}; callback names #{cbs.size}"
  puts "universe: #{core.size} core instance names (#{core.count { |n| rows.any? { |b, r, c| b == n && r == '*' && c } }} covered), " \
       "#{singles.size} singleton names (#{singles.count { |n| rows.any? { |b, r, c| b == n && r == '*' && c } }} covered)"
  puts "callbacks: #{cbs.sort.join(' ')}"
  puts "timed out: #{TIMED_OUT.map { |r, b| "#{r}#{b}" }.uniq.sort.join(' ')}"
end

def run_probes
  Dir.mktmpdir("bopcb") do |dir|
    Dir.chdir(dir) do
      probe_instances
      probe_subclasses
      probe_hooks
      probe_singletons
      probe_forked_all
      probe_output
      probe_kernel
    end
  end
end

def main
  opts = {}
  OptionParser.new do |p|
    p.on("--write") { opts[:write] = true }
    p.on("--check") { opts[:check] = true }
    p.on("--report") { opts[:report] = true }
  end.parse!

  run_probes
  rows = build_table
  text = generate(rows)

  if opts[:check]
    if File.exist?(OUTPUT) && File.read(OUTPUT) == text
      puts "bop-callbacks-check: ok (#{rows.size} rows)"
    else
      old = File.exist?(OUTPUT) ? File.read(OUTPUT).lines : []
      new = text.lines
      (new - old).first(10).each { |l| warn "bop-callbacks-check: + #{l}" }
      (old - new).first(10).each { |l| warn "bop-callbacks-check: - #{l}" }
      abort "bop-callbacks-check: src/builtin_callbacks.inc is stale; regenerate with --write"
    end
  elsif opts[:write]
    File.write(OUTPUT, text)
    puts "wrote #{OUTPUT}"
  elsif opts[:report]
    report(rows)
  else
    print text
  end
end

main if $PROGRAM_NAME == __FILE__
