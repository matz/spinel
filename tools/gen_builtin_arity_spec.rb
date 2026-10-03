#!/usr/bin/env ruby

# Generate the positional-arity spec tables in src/codegen_call.c
# (sp_builtin_arity_spec_tbl and sp_builtin_cmeth_arity_spec_tbl) by probing
# the local CRuby:
#
#   ruby tools/gen_builtin_arity_spec.rb            # print both tables
#   ruby tools/gen_builtin_arity_spec.rb --write    # splice them into
#                                                   # src/codegen_call.c
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

ROOT = File.expand_path("..", __dir__)
SOURCE = File.join(ROOT, "src/codegen_call.c")

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
PROBE_ANYWAY = { "StringIO" => %w[gets readline] }

# Class/module methods: the constructors and module functions whose emitters
# index argv[] unconditionally (File.open with no arguments crashed the
# compiler), the surface of the constants a program names bare, and the
# Kernel functions a bare call reaches. Every call is made with nil
# arguments, which Process.spawn, waitpid2 and the socket constructors
# reject before acting; still, never probe anything that could act on the
# probing process itself with any argument (fork / exec / select / sleep).
# GC.disable is undone by the GC.enable probed right after it.
CLASS_TARGETS = {
  "File"    => %w[open new read write binread binwrite readlines foreach delete unlink
                  rename exist? size basename dirname extname join split expand_path
                  chmod utime umask truncate symlink link readlink realpath stat lstat
                  ftype mtime atime ctime empty? zero? identical? absolute_path
                  file? directory? readable? writable? executable? symlink? size?
                  fnmatch fnmatch? owned? world_readable? world_writable? path
                  absolute_path? birthtime chown lchmod lchown mkfifo],
  # IO.select probes slowly (a nil-args call waits for the 2 s timeout) but
  # the row it yields is real: a missing-argument call is CRuby's
  # ArgumentError, and the probe never passes an actual IO to wait on.
  "IO"      => %w[for_fd sysopen new open read write binread binwrite readlines pipe
                  select copy_stream],
  "Dir"     => %w[new open mkdir rmdir delete unlink entries children glob foreach
                  exist? empty? home pwd getwd],
  "Time"    => %w[at local mktime utc gm now],
  "Hash"    => %w[new],
  "Array"   => %w[new],
  "String"  => %w[new],
  "Integer" => %w[sqrt],
  "Math"    => %w[sqrt cbrt sin cos tan asin acos atan atan2 sinh cosh tanh asinh acosh
                  atanh log log2 log10 exp hypot ldexp frexp erf erfc gamma lgamma],
  "Process" => %w[getpriority setpriority getsid kill clock_gettime clock_getres pid ppid
                  uid gid euid egid setproctitle times spawn waitpid2 wait2 wait waitpid],
  "Regexp"  => %w[new escape quote union],
  "Random"  => %w[new rand srand],
  "ENV"     => %w[fetch key? has_key? include? member? assoc rassoc values_at],
  "Kernel"  => %w[format sprintf Integer Float String Array Hash Rational Complex
                  throw catch raise rand srand caller lambda proc binding block_given?],
  # the class-side surface of the constants a program names bare: GC, Fiber
  # and Thread are not classes the compiler declares; a class Struct.new or
  # Data.define answered is keyed StructClass / DataClass
  "GC"      => %w[start stat compact count disable enable stress latest_gc_info],
  "Fiber"   => %w[current yield],
  "Thread"  => %w[current main list pass new start abort_on_exception
                  report_on_exception pending_interrupt? handle_interrupt],
  "StructClass" => %w[members keyword_init?],
  "DataClass"   => %w[members],
  "Marshal" => %w[dump load],
  # arity-only probing is safe here: every call is made with nil arguments,
  # which these constructors and entry points reject before acting
  "Signal"  => %w[signame list trap],
  "Socket"  => %w[new getaddrinfo getnameinfo pair socketpair gethostname sockaddr_in
                  unpack_sockaddr_in pack_sockaddr_in sockaddr_un pack_sockaddr_un],
  "TCPSocket"  => %w[new open],
  "TCPServer"  => %w[new open],
  "UNIXSocket" => %w[new open],
  "UNIXServer" => %w[new open],
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

# block: probe the counts of the block-carrying call ("2.step { }" accepts 0
# args where the bare call wants 1..; Array#fill drops to 0..2). The block
# used is inert but really runs, which is why the probe surface excludes
# anything that could act on the probing process.
def spec_of(recv, m, block: false)
  sym = m.to_sym
  return nil unless recv.call.respond_to?(sym)
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

# The bare spec plus the with-block spec in one 10-column row; a side the
# probe could not prove is the -1/NULL sentinel quartet (never fires).
NO_SPEC = [-1, -1, nil, nil]
def full_spec_of(recv, m)
  bare = spec_of(recv, m)
  blk = spec_of(recv, m, block: true)
  return nil if bare.nil? && blk.nil?
  [*(bare || NO_SPEC), *(blk || NO_SPEC)]
end

def render_table(name, header, entries)
  out = +""
  header.each_line { |l| out << l }
  out << "static const SpAritySpec\n#{name}[] = {\n"
  entries.each do |c, m, mn, mx, lo, hi, bmn, bmx, blo, bhi|
    q = ->(s) { s ? "\"#{s}\"" : "NULL" }
    out << %Q[  {"#{c}","#{m}",#{mn},#{mx},#{q.(lo)},#{q.(hi)},#{bmn},#{bmx},#{q.(blo)},#{q.(bhi)}},\n]
  end
  out << "  {NULL, NULL, 0, 0, NULL, NULL, 0, 0, NULL, NULL}\n};\n"
  out
end

src = File.read(SOURCE)
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
  unless cls == "Object"
    universal = Object.new.public_methods
    meths -= universal.map(&:to_s) - recv.class.instance_methods(false).map(&:to_s)
  end
  meths.each do |m|
    s = full_spec_of(thunk, m)
    inst << [cls, m, *s] if s
  end
  FORWARDING.fetch(cls, []).each { |m| inst << [cls, m, *NO_SPEC, *NO_SPEC] }
end
# one TyKind stands for Queue and SizedQueue both: keep the rows they agree on
sized = inst.select { |r| r[0] == "SizedQueue" }.map { |r| r[1..] }
inst.reject! { |r| r[0] == "SizedQueue" || (r[0] == "Queue" && !sized.include?(r[1..])) }

inst_hdr = <<~C
  /* Positional-arity spec for the builtin instance surface, probed from
     #{ver} by tools/gen_builtin_arity_spec.rb (see there for the
     technique; rerun it with --write to regenerate both tables). max -1 =
     no upper bound; a NULL exp = that side is never violated. The first
     quartet describes the bare call, the blk_ quartet the block-carrying
     call (several counts change under a block: Array#fill 1..3 vs 0..2,
     Integer#step); a quartet the probe could not prove, or a name whose
     counts are its target's (Proc#call, Enumerator#each), is
     -1,-1,NULL,NULL and never fires. */
C
inst_out = render_table("sp_builtin_arity_spec_tbl", inst_hdr, inst)

CLASS_CONSTANTS = {
  "StructClass" => Struct.new(:a), "DataClass" => Data.define(:a),
}
cm = []
CLASS_TARGETS.each do |cls, meths|
  const = CLASS_CONSTANTS.fetch(cls) { Object.const_get(cls) }
  thunk = -> { const }
  meths.each do |m|
    s = full_spec_of(thunk, m)
    cm << [cls, m, *s] if s
  end
end
cm_hdr = <<~C
  /* Class/module-method positional arity, probed from #{ver} the same
     way as the instance table above (tools/gen_builtin_arity_spec.rb): the
     constructors and module functions whose emitters index argv[]
     unconditionally (File.open with no arguments was a compile-time
     SIGSEGV), the surface of the constants a program names bare -- GC,
     Fiber, Thread, a class Struct.new or Data.define answered, keyed
     StructClass and DataClass -- and the Kernel functions a bare call
     reaches. */
C
cm_out = render_table("sp_builtin_cmeth_arity_spec_tbl", cm_hdr, cm)

Dir.chdir("/")   # leave the probe dir so it can be removed
FileUtils.remove_entry(PROBE_DIR) rescue nil

if ARGV.include?("--write")
  wrote = []
  { "instance" => [/\/\* Positional-arity spec for the builtin instance surface.*?\nsp_builtin_arity_spec_tbl\[\] = \{.*?\n\};\n/m, inst_out, inst.length],
    "class-method" => [/\/\* Class\/module-method positional arity.*?\nsp_builtin_cmeth_arity_spec_tbl\[\] = \{.*?\n\};\n/m, cm_out, cm.length],
  }.each do |label, (pat, replacement, count)|
    old = src[pat]
    if old
      src = src.sub(old, replacement)
      wrote << "#{count} #{label}"
    else
      warn "note: no existing #{label} spec table in #{SOURCE}; skipped"
    end
  end
  abort "no spec tables found in #{SOURCE}" if wrote.empty?
  File.write(SOURCE, src)
  warn "wrote #{wrote.join(" + ")} entries into #{SOURCE}"
else
  puts inst_out
  puts
  puts cm_out
  warn "#{inst.length} instance + #{cm.length} class-method entries"
end
