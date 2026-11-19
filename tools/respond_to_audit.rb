# respond_to? audit: for every (family, name) pair src/builtin_ops.c's
# bop_rows actually registers, does a BOXED sample receiver's
# respond_to?(:name) (lib/spinel_rt.h's sp_poly_responds_builtin) agree
# with CRuby's own answer?
#
#   ruby tools/respond_to_audit.rb [--ref RUBY]
#
# This exists because sp_poly_responds_builtin's per-class tables are a
# second, hand-maintained copy of what bop_rows already says is
# implemented (lib/spinel_rt.h's "uni"/"enumm"/"numm"/"intm"/"rngm"/"symm"/
# "procm"/"excm"/"regm"/"mdm" arrays, plus the generated
# "sp_gen_responds_array"/"hash"/"string"/"float" tables -- see
# tools/gen_respond_to_tables.rb), and nothing enforces the two stay in
# sync. They drifted once already: Array's table
# alone was missing 67 real methods, including every bang mutator (uniq!,
# sort!, compact!, ...), which is exactly the shape of bug gem "jaccard"
# hit for real (Jaccard.coefficient's `union.uniq! if
# union.respond_to?(:uniq!)` silently stopped deduping once `union` was
# boxed). Every sample receiver below is deliberately forced boxed (read
# back out of a 2-element mixed Array) for that reason: a CONCRETE,
# non-boxed receiver's respond_to? answer comes from an entirely
# different, unrelated mechanism (respond_to_static_answer /
# rt_probe_answer in src/codegen_call.c, which has its own gaps --
# tally/to_set/transform_keys!/unicode_normalize at least, found while
# writing this script -- tracked separately, NOT fixed here).
#
# Scope: 13 classes sp_poly_responds_builtin branches on -- Array,
# Hash, String, Integer, Float, Range, Symbol, Proc, Exception, Regexp,
# MatchData, Complex, Rational. Exception's
# per-subclass accessors (errno, key, receiver, name, result, ...) are
# gated at runtime by sp_exc_is_a (the boxed exception's own ancestor
# walk), not by a flat name list, since bop_rows has no owning-subclass
# field and a KeyError genuinely has #key while a plain RuntimeError does
# not; each family's FIRST sample in BuiltinRowGen::FAMILIES is a generic
# one (brp_key_error for Exception), so this only probes the names every
# exception shares -- the subclass-specific ones are covered by
# test/respond_to_builtin_mutator_surface.rb instead, not by this script.
# File/Queue/Mutex/Time/Random/Method/Enumerator
# go through other mechanisms (sp_io_typed_responds, or
# are not branched here at all) and are out of scope for this script.
# Rows tagged BOP_IVAR_LESS (compiler-internal reflection helpers like
# __bivar_get, never real Ruby-level methods) are excluded by name
# pattern, not by kind, since BOP_IVAR_LESS is shared with TY_INT/
# TY_STRING's real rows in BuiltinRowGen::FAMILIES.
#
# This is NOT a generator: a mismatch here means a human edits
# lib/spinel_rt.h's tables by hand, the same way the fix that prompted
# this script did. Generating the tables from bop_rows outright hits real
# obstacles for Range/Symbol/Exception (bop_rows is not complete for
# Range/Symbol -- their surface is desugared through Enumerable rather
# than a row per name -- and Exception registers subclass-specific
# accessors like `errno` with no "owning subclass" field to gate them);
# tools/gen_respond_to_tables.rb's check_range_coverage/check_symbol_
# coverage/check_exception_coverage self-checks instead re-verify, on
# every build, that whatever slice of each surface bop_rows *does*
# register stays covered by rngm[]/symm[]/excm[] -- this script remains
# the only thing that also covers the Enumerable-desugared and
# subclass-gated slices those self-checks structurally cannot reach.
#
# Exit status: 0 everyone agrees, 1 a mismatch, 4 the tool's own error.

require_relative "builtin_row_gen"
require "open3"
require "tmpdir"

ROOT = File.expand_path("..", __dir__)
SPINEL = ENV["SPINEL"] || File.join(ROOT, "bin/spinel")
ref_ruby = "ruby"
ARGV.each_with_index { |a, i| ref_ruby = ARGV[i + 1] if a == "--ref" }

# The 13 classes this script audits. Each entry
# is one BuiltinRowGen family name -> one sample receiver expression
# (reused from BuiltinRowGen::FAMILIES, first sample only: respond_to?
# does not care which instance, so one representative is enough).
SCOPE = %w[Array Hash String Integer Float Range Symbol Proc Exception Regexp MatchData Complex Rational].freeze

INTERNAL_NAME = BuiltinRowGen::INTERNAL_NAME

# Names bop_rows has for a typed Regexp / MatchData / Complex / Rational
# that a boxed one cannot be called with yet (the call raises
# NoMethodError, or for ~ does integer conversion instead of matching $_),
# so its table leaves them out on purpose: answering true would send a
# respond_to?-guarded call into that raise.
BOXED_UNCALLABLE = {
  "Regexp" => %w[encoding fixed_encoding? timeout ~],
  "MatchData" => %w[bytebegin byteend deconstruct deconstruct_keys match],
  "Complex" => %w[fdiv],
  "Rational" => %w[clamp to_c],
}.freeze

pairs = BuiltinRowGen.method_list.select do |family, name|
  SCOPE.include?(family) && !INTERNAL_NAME.match?(name) && !BOXED_UNCALLABLE.fetch(family, []).include?(name)
end
if pairs.empty?
  warn "respond_to_audit: no (family, name) pairs parsed -- bop_rows's shape changed?"
  exit 4
end

samples = SCOPE.to_h do |f|
  sample = BuiltinRowGen::FAMILIES.fetch(f).last&.first&.first
  if sample.nil?
    warn "respond_to_audit: no sample receiver for family #{f} -- FAMILIES shape changed?"
    exit 4
  end
  [f, sample]
end
helpers = BuiltinRowGen::HELPERS.values_at("brp_key_error").compact.join("\n")

lines = ["def force_poly(mixed, idx) = mixed[idx]", helpers]
lines.concat(pairs.map.with_index do |(family, name), i|
  "p(force_poly([(#{samples.fetch(family)}), +\"x\"], 0).respond_to?(#{name.to_sym.inspect})) # #{i} #{family}##{name}"
end)
program = "#{lines.join("\n")}\n"

Dir.mktmpdir("respond_to_audit") do |dir|
  src = File.join(dir, "probe.rb")
  File.write(src, program)

  ref_out, ref_err, ref_st = Open3.capture3(ref_ruby, src)
  unless ref_st.success?
    warn "respond_to_audit: the probe program itself failed under #{ref_ruby}:\n#{ref_err}"
    exit 4
  end

  bin = File.join(dir, "probe")
  _, sp_err, sp_cst = Open3.capture3(SPINEL, src, "-o", bin)
  unless sp_cst.success?
    warn "respond_to_audit: #{SPINEL} failed to compile the probe:\n#{sp_err}"
    exit 4
  end
  sp_out, sp_err, sp_st = Open3.capture3(bin)
  unless sp_st.success?
    warn "respond_to_audit: the compiled probe failed to run:\n#{sp_err}"
    exit 4
  end

  ref_lines = ref_out.lines
  sp_lines = sp_out.lines
  if ref_lines.size != sp_lines.size
    warn "respond_to_audit: line count mismatch (#{ref_ruby}: #{ref_lines.size}, spinel: #{sp_lines.size})"
    exit 4
  end

  mismatches = []
  pairs.each_with_index do |(family, name), i|
    next if ref_lines[i] == sp_lines[i]
    mismatches << "  ##{i} #{family}##{name}: #{ref_ruby}=#{ref_lines[i].chomp} spinel=#{sp_lines[i].chomp}"
  end

  if mismatches.empty?
    puts "respond_to_audit: #{pairs.size} (family, name) pairs agree with #{ref_ruby}"
    exit 0
  else
    warn "respond_to_audit: #{mismatches.size}/#{pairs.size} mismatches"
    warn mismatches.join("\n")
    exit 1
  end
end
