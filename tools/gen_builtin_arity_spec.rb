#!/usr/bin/env ruby

# Generate shared builtin arity facts in src/builtin_arity.inc from local
# CRuby. Method#arity is read directly; positional counts are probed.
# codegen_call.c projects the two independent facts into its three tables.
#
#   ruby tools/gen_builtin_arity_spec.rb            # print the shared rows
#   ruby tools/gen_builtin_arity_spec.rb --write    # regenerate the rows
#   ruby tools/gen_builtin_arity_spec.rb --check    # fail when the source
#                                                   # differs (make arity-spec-check)
#
# The builtin-op rows' argc_min/argc_max (builtin_ops.c) are checked against
# the instance spec table, and the Method#arity table against it too, by
# `spinel --check-bop-arity` (make bop-arity-check-test, in the gate).
#
# Probing technique, per (class, method):
#
#   1. Counts 0..3 are called with nil arguments to locate the true MINIMUM.
#      An outcome other than a "wrong number of arguments" ArgumentError -- a
#      TypeError, a RangeError, success -- proves the COUNT is accepted (the
#      values were the problem). This must run on NON-EMPTY receivers: several
#      C methods take an empty-receiver fast path that returns before the
#      arity check ("".delete never raises), which would hide the method.
#   2. A 99-argument call makes CRuby name the accepted range in its own
#      message ("given 99, expected 0..1"), yielding the MAXIMUM and CRuby's
#      exact wording. The low-side message is captured separately from a
#      (min-1)-argument call, because branch-implemented methods word the two
#      sides differently (Range#first: 0 args is a bare read, 1.. args is
#      "expected 1").
#   3. Every count 0..12 is then VERIFIED against the derived min/max. A
#      method whose accepted set is not one contiguous range (Time.utc takes
#      1..8 OR exactly 10) is left out -- unguarded -- rather than guessed.
#
# The instance surface probed is every public method of each receiver below;
# the class-method surface is the curated list. Anything the probe cannot
# prove is omitted, so the guard the tables drive fires only where CRuby
# itself would raise.
#
# Every method is probed twice: bare, and carrying an inert block ("2.step { }"
# accepts 0 args where the bare call wants a limit; Array#fill drops to 0..2).
# Each row holds both quartets; an unprovable side is the -1 sentinel quartet.

# The libraries below add methods of their own, and a newer gem than the one
# CRuby ships changes them (json's generic to_json and parse arities, time's
# rfc3339), so the tables would depend on what else is installed. Activate the
# default versions first; stop when one cannot be, rather than probe whatever
# loads.
%w[json time].each do |name|
  spec = Gem::Specification.find_all_by_name(name).find(&:default_gem?)
  abort "#{name}: the default gem of this CRuby is not installed" unless spec
  begin
    gem name, spec.version.to_s
  rescue Gem::LoadError => e
    abort "#{name}: cannot activate the default gem: #{e.message}"
  end
end

require "timeout"
require "fileutils"
require "set"
require "socket"
require "csv"
require "json"
require "base64"
require "digest"
require "stringio"
require "strscan"
require "pathname"

Warning[:deprecated] = false  # probing deprecated arg shapes is the point
$VERBOSE = nil                # and unused blocks, superseded defaults (warn is
                              # silent from here: report with $stderr.puts)

ROOT = File.expand_path("..", __dir__)
SOURCE = File.join(ROOT, "src/builtin_arity.inc")

# Non-empty receivers (see note above), built FRESH per probe: a shared
# receiver is mutated by the probes themselves (StringIO#reopen left every
# later method probing a dead stream, losing its row).
INSTANCE_RECEIVERS = {
  "String" => -> { "ab".dup }, "Integer" => -> { 1 }, "Float" => -> { 1.0 },
  "Symbol" => -> { :a }, "Array" => -> { [1, 2] }, "Hash" => -> { {1 => 2} },
  "Range" => -> { (1..2) }, "Time" => -> { Time.at(0) },
  "NilClass" => -> { nil }, "TrueClass" => -> { true },
  "Rational" => -> { 1r }, "Complex" => -> { 1i }, "Object" => -> { Object.new },
  # native (package-backed) classes: their loose C dispatch dropped excess
  # arguments (StringIO#eof?(1) answered false), so the guard covers them too
  "StringIO" => -> { StringIO.new("ab".dup) },
  "StringScanner" => -> { StringScanner.new("ab") },
  "Pathname" => -> { Pathname.new("a") }, "Set" => -> { Set.new([1, 2]) },
  "Mutex" => -> { Mutex.new },
  # the handle kinds the guard maps by TyKind (emit_builtin_arity_guard): a
  # row must hold for every value of the kind, so Queue's rows are those
  # Queue and SizedQueue agree on (one TyKind stands for both), the IO rows
  # are probed on a File (the surface an IO handle can answer), and Struct
  # and Data instances lose their own members' accessors below
  "Regexp" => -> { /a/ }, "MatchData" => -> { "ab".match(/a/) },
  "Proc" => -> { proc { } }, "Method" => -> { 1.method(:to_s) },
  "Fiber" => -> { Fiber.new { } }, "Thread" => -> { Thread.new { }.join },
  "Queue" => -> { Queue.new([1, 2]) },
  "SizedQueue" => -> { q = SizedQueue.new(4); q << 1; q },
  "ConditionVariable" => -> { ConditionVariable.new },
  # a handle per probe would run the process out of descriptors: close the last
  "File" => -> { ($probe_io&.close rescue nil)
                 $probe_io = File.open("f", "w+"); $probe_io.write("ab"); $probe_io.rewind; $probe_io },
  "Dir" => -> { ($probe_dir&.close rescue nil); $probe_dir = Dir.new(".") },
  "Exception" => -> { RuntimeError.new("x") },
  "Enumerator" => -> { [1, 2].each }, "Random" => -> { Random.new(1) },
  "Struct" => -> { Struct.new(:a).new(1) },
  "Data" => -> { Data.define(:a).new(a: 1) },
  "Class" => -> { Class.new }, "Module" => -> { Module.new },
}

# Names whose accepted counts belong to what the receiver forwards to, not
# to its class: Method#call is its target's, Enumerator#each and to_a hand
# their arguments to the call underneath, Class#new is initialize's. Each
# keeps a row that never fires (the sentinel quartets), so the guard sees the
# class owns the name and does not read Object's row for it -- Proc#=== is
# an Object name too. A probe Struct's or Data's member accessors are the
# receiver's, not the class's, and get no row at all.
FORWARDING = {
  "Proc" => %w[call [] === yield], "Method" => %w[call [] ===],
  "Enumerator" => %w[each to_a entries to_h each_entry reverse_each],
  "Class" => %w[new],
}
PROBE_ARTEFACTS = { "Struct" => %w[a a=], "Data" => %w[a] }

# Private instance methods a class body reaches receiverless, probed like the
# public surface (the unrouted-call gate asks whether the row admits the
# call's count): Module#ruby2_keywords.
PRIVATE_INSTANCE = { "Module" => %w[ruby2_keywords] }

# The surface probed per class: every public instance method (the arity dump
# alone missed Float#div, Integer#step, ...), minus anything that could act
# on the probing process or its I/O.
INSTANCE_METHOD_SKIP = %w[
  exit exit! fork system exec spawn sleep gets readline trap syscall
  display print puts pp p warn require require_relative load autoload
  instance_eval instance_exec class_eval module_eval eval
  taint untaint
  pread pwrite sysread syswrite readpartial read_nonblock write_nonblock
  sysseek ioctl fcntl
  set_encoding_by_bom
]
# pread (with mistyped arguments) and set_encoding_by_bom (with a block)
# each segfault CRuby 4.0.6 itself when probed; both stay off the surface.
# A class's own reader of its in-memory stream is safe to probe, though the
# names are skipped everywhere else (Kernel#gets reads stdin).
PROBE_ANYWAY = { "StringIO" => %w[gets readline], "Mutex" => %w[sleep] }
# (Mutex#sleep on the unlocked probe mutex raises ThreadError before it
# waits, for every count it accepts.)

# Class/module methods: the constructors and module functions whose emitters
# index argv[] unconditionally (File.open with no arguments crashed the
# compiler), the surface of the constants a program names bare, and the
# Kernel functions a bare call reaches. Every probed call is made with nil
# arguments, which Process.spawn, waitpid2 and the socket constructors
# reject before acting. A name that could act on the probing process (fork,
# exec, sleep, Thread.stop) stays in this list for its place in the table but
# is not probed: it goes through CLASS_UNCOUNTED, CLASS_INTROSPECTED or
# CLASS_BELOW_MIN below, and is never called with a count it accepts. (IO.select
# is probed: it only waits, and never gets an IO to wait on.)
# GC.disable is undone by the GC.enable probed right after it.
CLASS_TARGETS = {
  "File"    => %w[open new read write binread binwrite readlines delete unlink
                  rename exist? size basename dirname extname join split expand_path
                  chmod utime umask truncate symlink link readlink realpath stat lstat
                  ftype mtime atime ctime empty? zero? identical? absolute_path
                  file? directory? readable? writable? executable? symlink? size?
                  fnmatch fnmatch? owned? grpowned? setuid? setgid? sticky? pipe?
                  socket? blockdev? chardev? world_readable? world_writable? path
                  absolute_path? birthtime chown lchmod lchown mkfifo],
  # IO.select probes slowly (a nil-args call waits for the 2 s timeout) but
  # the row it yields is real: a missing-argument call is CRuby's
  # ArgumentError, and the probe never passes an actual IO to wait on.
  # (foreach is listed here: the table's row for it is IO's, where #10119 put it)
  "IO"      => %w[for_fd sysopen new open read write binread binwrite readlines pipe
                  select copy_stream foreach try_convert],
  "Dir"     => %w[new open mkdir rmdir delete unlink entries children glob foreach
                  exist? empty? home pwd getwd],
  "Time"    => %w[at local mktime utc gm now],
  "Hash"    => %w[new try_convert []],
  "Array"   => %w[new try_convert []],
  "String"  => %w[new try_convert],
  "Integer" => %w[sqrt try_convert],
  "Math"    => %w[sqrt cbrt sin cos tan asin acos atan atan2 sinh cosh tanh asinh acosh
                  atanh log log2 log10 exp hypot ldexp frexp erf erfc gamma lgamma],
  "Process" => %w[getpriority setpriority getsid kill clock_gettime clock_getres pid ppid
                  uid gid euid egid setproctitle times spawn waitpid2 wait2 wait waitpid
                  exec last_status],
  "Regexp"  => %w[new escape quote try_convert last_match union],
  "Random"  => %w[new rand srand],
  "ENV"     => %w[fetch key? has_key? include? member? assoc rassoc values_at],
  "Kernel"  => %w[format sprintf Integer Float String Array Hash Rational Complex
                  throw catch raise rand srand caller lambda proc binding block_given?
                  local_variables global_variables respond_to_missing?],
  # the class-side surface of the constants a program names bare: GC, Fiber
  # and Thread are not classes the compiler declares; a class Struct.new or
  # Data.define answered is keyed StructClass / DataClass
  "GC"      => %w[start stat compact count disable enable stress latest_gc_info],
  "Fiber"   => %w[current yield],
  "Thread"  => %w[current main list pass new stop start fork abort_on_exception
                  report_on_exception report_on_exception= pending_interrupt?
                  handle_interrupt],
  "StructClass" => %w[members keyword_init?],
  "DataClass"   => %w[members],
  "Marshal" => %w[dump load],
  # arity-only probing is safe here: every call is made with nil arguments,
  # which these constructors and entry points reject before acting
  "Signal"  => %w[signame list trap],
  "Socket"  => %w[new getaddrinfo getnameinfo pair socketpair gethostname sockaddr_in
                  unpack_sockaddr_in pack_sockaddr_in sockaddr_un pack_sockaddr_un],
  # (BasicSocket's own class methods; for_fd is ahead of IO.for_fd)
  "BasicSocket" => %w[for_fd do_not_reverse_lookup do_not_reverse_lookup=],
  "TCPSocket"  => %w[new open],
  "TCPServer"  => %w[new open],
  "UNIXSocket" => %w[new open],
  "UNIXServer" => %w[new open],
  "UDPSocket"  => %w[new],
  "SizedQueue" => %w[new],
  "Queue"      => %w[new],
  "Base64"  => %w[encode64 decode64 strict_encode64 strict_decode64
                  urlsafe_encode64 urlsafe_decode64],
  "CSV"     => %w[new parse parse_line generate generate_line foreach read readlines open],
  "JSON"    => %w[parse generate dump load pretty_generate],
  "Digest::MD5"    => %w[hexdigest digest base64digest file],
  "Digest::SHA1"   => %w[hexdigest digest base64digest file],
  "Digest::SHA256" => %w[hexdigest digest base64digest file],
  "StringIO"      => %w[new open],
  "StringScanner" => %w[new],
  "Pathname"      => %w[new glob getwd pwd],
}

# CLASS_TARGETS names that are not probed the usual way: a call could act on
# the probing process, or the probe would find nothing to enforce. They keep
# their place in CLASS_TARGETS (the rows follow its order) and take their row
# from Method#arity and Method#parameters instead (CLASS_BELOW_MIN also makes
# two harmless calls, below).
#
# CLASS_UNCOUNTED rows are the never-firing sentinel quartets, like
# FORWARDING's: the method takes any number of arguments, so there is no
# count to guard, but the row tells the guard the class owns the name
# (singleton_methods(false), respond_to?). Every C method of arity -1
# reports [[:rest]], so that check alone proves little: these three are
# harmless to call, so the run also probes them and aborts unless the probe
# finds no count to enforce, which catches a future check in them.
#   Hash.[], Array.[], Regexp.union  variadic constructors, nothing to enforce
CLASS_UNCOUNTED = { "Hash" => %w[[]], "Array" => %w[[]], "Regexp" => %w[union] }
# CLASS_UNCOUNTED_UNCALLED rows are the same, for names that spawn a thread
# and so are not called at all: the [[:rest]] parameters check is all there
# is for them.
#   Thread.start, Thread.fork
CLASS_UNCOUNTED_UNCALLED = { "Thread" => %w[start fork] }
# CLASS_INTROSPECTED rows are read off Method#parameters, which must be
# required positionals only (the accepted count is then exactly their number
# and CRuby words both sides as that number).
#   Thread.stop                      halts the probing thread
#   Thread.report_on_exception=      changes the process's thread reporting
CLASS_INTROSPECTED = { "Thread" => %w[stop report_on_exception=] }
# CLASS_BELOW_MIN entries are safe to call only with fewer arguments than
# their minimum, because CRuby checks the count before acting: the ArgumentError
# is raised before anything happens. Only count 0 is ever called; its message
# names the minimum, and the maximum comes from Method#parameters, which must
# be a lone *rest (unbounded). An accepted count is never called.
#   Process.exec                     replaces the probing process once it
#                                    accepts its arguments
CLASS_BELOW_MIN = { "Process" => %w[exec] }

# A top-level def is a private method of every object, the probe receivers
# included, and a builtin's internals may dispatch to it -- an Enumerator's
# size asks its target for `call` -- so these helpers keep names no
# builtin sends.
def probe_counts(thunk, m, n, block: false)
  r2 = thunk.call
  Timeout.timeout(2) do
    block ? r2.__send__(m, *Array.new(n)) { |*| "a" } : r2.__send__(m, *Array.new(n))
  end
  :ok
rescue ArgumentError => e
  e.message[/wrong number of arguments \(given #{n}, expected ([^)]+)\)/, 1] || :ok
rescue Exception
  :ok  # the count was accepted; the values (or environment) were not
end

def kw_positional?(thunk, m, max, block: false)
  r2 = thunk.call
  Timeout.timeout(2) do
    block ? r2.__send__(m, *Array.new(max), __kwprobe: 1) { |*| "a" } : r2.__send__(m, *Array.new(max), __kwprobe: 1)
  end
  false
rescue ArgumentError => e
  e.message.start_with?("wrong number of arguments (given #{max + 1},")
rescue Exception
  false
end

# block: probe the counts of the block-carrying call ("2.step { }" accepts 0
# args where the bare call wants 1..; Array#fill drops to 0..2). The block
# used is inert but really runs, which is why the probe surface excludes
# anything that could act on the probing process.
def spec_of(recv, m, block: false, private_ok: false)
  sym = m.to_sym
  return nil unless recv.call.respond_to?(sym, private_ok)
  low = (0..3).map { |n| probe_counts(recv, sym, n, block: block) }
  min = low.index(:ok)
  return nil if min.nil?  # needs > 3 required args: not on this surface
  hi = probe_counts(recv, sym, 99, block: block)
  max, hi_exp =
    case hi
    when :ok then [-1, nil]
    when /\A\d+\z/, /\A\d+\.\.\d+\z/ then [hi[/(\d+)\z/, 1].to_i, hi]
    when /\A\d+\+\z/ then [-1, nil]
    else return nil  # keyword-argument wording: leave the method unguarded
    end
  return nil if min == 0 && max == -1  # nothing to enforce
  lo_exp = min > 0 && low[min - 1].is_a?(String) ? low[min - 1] : nil
  # a minimum the probe proved but whose message we could not parse: skip the
  # low side rather than guess a message
  min = 0 if min > 0 && lo_exp.nil?
  return nil if min == 0 && max == -1
  # the spec must predict every count: a gapped acceptance set (Time.utc,
  # String#bytesplice) keeps only its provable LOW side (max unenforced) --
  # every count below min must still be rejected with the same wording
  (0..12).each do |n|
    predicted_ok = n >= min && (max < 0 || n <= max)
    actual = n <= 3 ? low[n] : probe_counts(recv, sym, n, block: block)
    next if (actual == :ok) == predicted_ok
    return nil unless min > 0 && lo_exp
    (0...min).each { |k| return nil if low[k] == :ok }
    return [min, -1, lo_exp, nil]
  end
  [min, max, lo_exp, hi_exp]
end

# The spec of a CLASS_UNCOUNTED* / CLASS_INTROSPECTED method, from the method
# object alone. Anything but the shapes documented there aborts the run,
# rather than record a row the method no longer has.
def declared_spec_of(const, m, kind)
  params = const.method(m).parameters
  if kind != :introspected
    abort "#{const}.#{m} is no longer a lone *rest: #{params.inspect}" unless params == [[:rest]]
    if kind == :uncounted && full_spec_of(-> { const }, m)
      abort "#{const}.#{m} now has a count to enforce: probe it as an ordinary target"
    end
    return [*NO_SPEC, *NO_SPEC]
  end
  abort "#{const}.#{m} is not required positionals only: #{params.inspect}" unless params.all? { |k, _| k == :req }
  n = params.length
  [n, n, (n.to_s if n > 0), n.to_s] * 2
end

# The spec of a CLASS_BELOW_MIN method: the zero-argument call, bare and with
# a block, must raise the count ArgumentError "expected N+" (N > 0); count 0
# is the only count called.
def below_min_spec_of(const, m)
  params = const.method(m).parameters
  abort "#{const}.#{m} is no longer a lone *rest: #{params.inspect}" unless params == [[:rest]]
  sides = [false, true].map do |block|
    begin
      block ? const.__send__(m) { "a" } : const.__send__(m)
    rescue ArgumentError => e
      msg = e.message[/\Awrong number of arguments \(given 0, expected (\d+\+)\)\z/, 1]
      abort "#{const}.#{m}: unexpected zero-argument message #{e.message.inspect}" unless msg && msg.to_i > 0
      next [msg.to_i, -1, msg, nil]
    rescue Exception => e
      abort "#{const}.#{m}: the zero-argument call raised #{e.class}, not the count error: #{e.message}"
    end
    abort "#{const}.#{m} accepted zero arguments: not a below-minimum probe"
  end
  sides.flatten(1)
end

# The bare spec plus the with-block spec in one 10-column row; a side the
# probe could not prove is the -1/NULL sentinel quartet (never fires).
NO_SPEC = [-1, -1, nil, nil]
def full_spec_of(recv, m, private_ok: false)
  bare = spec_of(recv, m, private_ok: private_ok)
  blk = spec_of(recv, m, block: true, private_ok: private_ok)
  return nil if bare.nil? && blk.nil?
  [*(bare || NO_SPEC), *(blk || NO_SPEC)]
end

if $PROGRAM_NAME == __FILE__
# ver names this run's CRuby in the messages only. The header says CRuby 4.0:
# the tables do not depend on the patch release, so a check run on another
# patch release of 4.0 must not fail on it.
ver = RUBY_DESCRIPTION.split(" (").first

# Probe inside a throwaway directory: the Pathname receiver is the relative
# path "a", and its rmtree / delete / mkdir / write probes act on the real
# filesystem -- run anywhere else they would destroy an unrelated "./a".
require "tmpdir"
PROBE_DIR = Dir.mktmpdir("arity_probe")
Dir.chdir(PROBE_DIR)

inst = []
INSTANCE_RECEIVERS.each do |cls, thunk|
  recv = thunk.call
  meths = recv.public_methods.map(&:to_s).sort -
          (INSTANCE_METHOD_SKIP - PROBE_ANYWAY.fetch(cls, [])) -
          FORWARDING.fetch(cls, []) - PROBE_ARTEFACTS.fetch(cls, [])
  # Object's universal surface is carried by the "Object" rows; the per-class
  # rows keep only what the class itself (or its non-Object ancestry) defines,
  # so the table stays deduplicated and the guard falls back explicitly.
  # Probe subclasses still inherit their family's own methods: the owner,
  # rather than the probe class's direct definitions, decides that boundary.
  unless cls == "Object"
    meths.reject! { |m| Object.ancestors.include?(recv.method(m).owner) }
  end
  meths.each do |m|
    s = full_spec_of(thunk, m)
    inst << [cls, m, *s] if s
  end
  PRIVATE_INSTANCE.fetch(cls, []).each do |m|
    s = full_spec_of(thunk, m, private_ok: true)
    inst << [cls, m, *s] if s
  end
  FORWARDING.fetch(cls, []).each { |m| inst << [cls, m, *NO_SPEC, *NO_SPEC] }
end
# one TyKind stands for Queue and SizedQueue both: keep the rows they agree
# on, and where SizedQueue's counts take in Queue's (push's non_block flag:
# 1 against 1..2), SizedQueue's row, which fires only for a count neither
# class takes
def spec_covers(wide, narrow)
  [0, 4].all? do |o|
    wmin, wmax, nmin, nmax = wide[o], wide[o + 1], narrow[o], narrow[o + 1]
    next wmin < 0 if nmin < 0
    wmin >= 0 && wmin <= nmin && (wmax < 0 || (nmax >= 0 && nmax <= wmax))
  end
end
sized = inst.select { |r| r[0] == "SizedQueue" }.to_h { |r| [r[1], r[2..]] }
inst.reject! { |r| r[0] == "SizedQueue" }
inst.map! do |r|
  next r unless r[0] == "Queue"
  s = sized[r[1]]
  next nil unless s
  next r if s == r[2..]
  spec_covers(s, r[2..]) ? ["Queue", r[1], *s] : nil
end
inst.compact!


CLASS_CONSTANTS = {
  "StructClass" => Struct.new(:a), "DataClass" => Data.define(:a),
}
cm = []
CLASS_TARGETS.each do |cls, meths|
  const = CLASS_CONSTANTS.fetch(cls) { Object.const_get(cls) }
  thunk = -> { const }
  meths.each do |m|
    declared = [[CLASS_UNCOUNTED, :uncounted], [CLASS_UNCOUNTED_UNCALLED, :uncounted_uncalled],
                [CLASS_INTROSPECTED, :introspected]].find { |t, _| t.fetch(cls, []).include?(m) }
    s = if CLASS_BELOW_MIN.fetch(cls, []).include?(m) then below_min_spec_of(const, m)
        elsif declared then declared_spec_of(const, m, declared[1])
        else full_spec_of(thunk, m, private_ok: cls == "Kernel")
        end
    cm << [cls, m, *s] if s
  end
end
# The class methods whose keyword hash is one more positional (BACK): a
# fixed Method#arity and no keyword parameters, as BasicSocket.for_fd(fd)
# has, so `BasicSocket.for_fd(1, autoclose: false)` passes two arguments
cmkw = cm.select do |cls, m, *|
  meth = CLASS_CONSTANTS.fetch(cls) { Object.const_get(cls) }.method(m)
  meth.arity >= 0 && meth.parameters.none? { |kind, _| %i[key keyreq keyrest].include?(kind) }
end

# The libraries whose methods on a builtin class Spinel answers from the
# library's builtin-op rows (io/console: IO#winsize, IO.console). Each is
# loaded after the core surface above is probed, and what it adds -- the
# public methods a probe receiver answers after the require and not before,
# and the class methods a class itself defines then -- is probed and written
# as BAL (instance) and BALC (class) rows that name the library first. Only
# the count check reads them, for a program that has the library; the core
# tables (names, reflection) stay the core's.
LIBRARIES = %w[io/console]
lib = []
libc = []
LIBRARIES.each do |feature|
  inst_before = INSTANCE_RECEIVERS.to_h { |cls, thunk| [cls, thunk.call.public_methods] }
  consts = CLASS_TARGETS.keys.to_h { |cls| [cls, CLASS_CONSTANTS.fetch(cls) { Object.const_get(cls) }] }
  cls_before = consts.transform_values(&:public_methods)
  require feature
  INSTANCE_RECEIVERS.each do |cls, thunk|
    added = thunk.call.public_methods - inst_before[cls]
    (added.map(&:to_s).sort - INSTANCE_METHOD_SKIP).each do |m|
      s = full_spec_of(thunk, m)
      lib << [feature, cls, m, *s] if s
    end
  end
  consts.each do |cls, const|
    added = (const.public_methods - cls_before[cls]).select { |m| const.method(m).owner == const.singleton_class }
    added.map(&:to_s).sort.each do |m|
      s = full_spec_of(-> { const }, m)
      libc << [feature, cls, m, *s] if s
    end
  end
end
# Method#arity of each class's OWN public instance methods, read straight off
# CRuby rather than probed (a C method that counts its own arguments reads
# -1 here; the spec tables above hold what it accepts), plus the Kernel
# functions the receiverless `method(:name)` wrapper binds under "Kernel".
ARITY_CLASSES = %w[String Integer Float Array Hash Symbol Range Time]
ARITY_KERNEL = %w[String Integer Float Array Rational Complex puts print p pp]
# Method#parameters of the same methods, read off instance_method by the
# class or module that defines each (Method#owner), and whether its
# source_location is nil (a method CRuby writes in C): the classes' own,
# the Kernel functions above, and the public methods of the modules a
# builtin class has its other methods from
PARAM_MODULES = %w[Numeric Comparable Kernel BasicObject]

# read in a fresh interpreter with no gems: the libraries this probe loads
# add methods of their own (csv's String#parse_csv, time's Time#httpdate)
require "rbconfig"
arity_src = <<~RUBY
  #{ARITY_CLASSES.inspect}.each do |cls|
    k = Object.const_get(cls)
    k.public_instance_methods(false).sort.each { |m| puts [cls, m, k.instance_method(m).arity].join("\\t") }
  end
  #{ARITY_KERNEL.inspect}.each { |m| puts ["Kernel", m, Kernel.instance_method(m).arity].join("\\t") }
  enc = ->(k, m) do
    um = k.instance_method(m)
    ps = um.parameters.map { |kind, name| name ? "\#{kind}:\#{name}" : kind.to_s }.join(" ")
    puts ["P", k, m, ps, um.source_location.nil? ? 1 : 0].join("\\t")
  end
  #{ARITY_CLASSES.inspect}.each { |cls| k = Object.const_get(cls); k.public_instance_methods(false).sort.each { |m| enc.(k, m) } }
  #{PARAM_MODULES.inspect}.each do |mod|
    k = Object.const_get(mod)
    ms = k.public_instance_methods(false)
    ms |= #{ARITY_KERNEL.inspect}.map(&:to_sym) if k == Kernel
    ms.sort.each { |m| enc.(k, m) }
  end
RUBY
dump = IO.popen([RbConfig.ruby, "--disable-gems", "-e", arity_src], &:read).lines.map { |l| l.chomp.split("\t", -1) }
abort "Method#arity dump failed" unless $?.success? && !dump.empty?
params, arity_all = dump.partition { |r| r[0] == "P" }
arity_all.map! { |c, m, a| [c, m, a.to_i] }
kernel_arity, arity = arity_all.partition { |r| r[0] == "Kernel" }
kwpos = inst.select do |cls, m, min, max, _, _, bmin, bmax|
  sides = [[min, max, false], [bmin, bmax, true]].select { |lo, hi, _| lo >= 0 && hi >= 0 }
  thunk = INSTANCE_RECEIVERS.fetch(cls)
  next false if sides.empty? || (thunk.call.method(m).arity rescue 0) >= 0
  sides.all? { |_, hi, blk| kw_positional?(thunk, m.to_sym, hi, block: blk) }
end
# Keep both facts where available; introspection is not an acceptance rule.
# Retain Method#arity order, including the receiverless Kernel wrappers.
counts = inst.to_h { |r| [r[0, 2], r[2..]] }
out = <<~C
  /* Generated by tools/gen_builtin_arity_spec.rb from CRuby 4.0: do not edit;
     add a new method to the generator's lists and regenerate with --write.
     Method#arity and accepted positional counts are independent facts.
     BAI carries both, BAM only Method#arity, BAS only instance counts,
     and BAC class/module counts; BAL and BALC are a library's instance
     and class counts, its name first. BACK names the class methods whose
     keyword hash counts as a positional (fixed arity, no keywords). -1 maxima are unbounded; NULL messages
     and unproved -1 minima never reject a call. Block counts are separate.
     Kernel Method#arity describes receiverless method(:name) wrappers.
     BAP is Method#parameters by the class or module that defines the
     method, as "kind[:name]" entries separated by spaces, and 1 where its
     source_location is nil (CRuby writes it in C). */
C
render = ->(tag, values) do
  fields = values.map { |v| v.nil? ? "NULL" : v.is_a?(String) ? v.dump : v.to_s }
  out << "#{tag}(#{fields.join(",")})\n"
end
(arity + kernel_arity).each do |r|
  spec = counts.delete(r[0, 2])
  render.(spec ? "BAI" : "BAM", spec ? r + spec : r)
end
inst.each { |r| render.("BAS", r) if counts.key?(r[0, 2]) }
cm.each { |r| render.("BAC", r) }
lib.each { |r| render.("BAL", r) }
libc.each { |r| render.("BALC", r) }
kwpos.each { |r| render.("BAK", r[0, 2]) }
cmkw.each { |r| render.("BACK", r[0, 2]) }
params.each { |_, k, m, ps, cside| render.("BAP", [k, m, ps, cside.to_i]) }
summary = "#{arity.length + kernel_arity.length} Method#arity + #{inst.length} instance + #{cm.length} class-method + #{lib.length + libc.length} library + #{params.length} parameters entries"
if ARGV.include?("--check")
  # the private-Kernel list the compiler and the runtime share is held to CRuby's
  listed = File.read(File.join(__dir__, "../lib/sp_kernel_private.h")).scan(/X\("([^"]+)"\)/).flatten
  stray = listed - Kernel.private_instance_methods.map(&:to_s)
  abort "lib/sp_kernel_private.h names methods #{ver}'s Kernel does not define private: #{stray.join(", ")}" unless stray.empty?
  if File.read(SOURCE) == out
    $stderr.puts "arity facts match #{ver}: #{summary}"
    exit 0
  end
  require "tempfile"
  Tempfile.create("arity") do |f|
    f.write(out)
    f.flush
    system("diff", "-u", "--label", "src/builtin_arity.inc", "--label", "regenerated", SOURCE, f.path)
  end
  abort "arity facts drifted from #{ver}: rerun tools/gen_builtin_arity_spec.rb --write"
elsif ARGV.include?("--write")
  File.write(SOURCE, out)
  $stderr.puts "wrote #{summary} into #{SOURCE}"
else
  puts out
  $stderr.puts summary
end
end
