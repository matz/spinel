#!/usr/bin/env ruby
# frozen_string_literal: true
#
# Generates lib/spinel_rt_responds_generated.h: the respond_to? reflection
# arrays for the five classes whose bop_rows[] coverage is complete enough
# to generate from (Proc/Range/Symbol/Exception's self-checks below cover
# the other four classes sp_poly_responds_builtin branches on that aren't
# generated, for the structural reasons each check's own comment gives).
#
#   ruby tools/gen_respond_to_tables.rb > lib/spinel_rt_responds_generated.h
#
# The sources it reads are UTF-8 whatever the locale: under LC_ALL=C (a bare
# container) Ruby read them as US-ASCII and the build stopped at the first
# non-ASCII byte (#9190).
Encoding.default_external = Encoding::UTF_8
require_relative "builtin_row_gen"

module GenRespondToTables
  ROOT = File.expand_path("..", __dir__)
  # bop_rows families this generator replaces the hand-written array for.
  # Proc is deliberately absent: its hand-written procm[] already covers
  # every bop_rows row that isn't already in the universal uni[] list (see
  # the spec), so there is nothing to generate -- only to keep checking.
  FAMILIES = %w[Array Hash String Integer Float].freeze

  # [family, name] pairs BuiltinRowGen.method_list reports that are not
  # actually true: BuiltinRowGen::FAMILIES attributes every TY_POLY row to
  # String (and Symbol), because most TY_POLY rows genuinely are String's
  # boxed face (capitalize, downcase, dump, swapcase, undump, upcase). The
  # 2-argument "pack" row under TY_POLY is Array#pack's buffer-argument
  # dispatch variant, not a String method -- CRuby: "".respond_to?(:pack)
  # is false. BuiltinRowGen's own use (probing call shapes on real
  # receivers) never hits this, since it never builds a String sample to
  # call "pack" on; this generator asks "is this really true of the
  # class," where the misattribution matters.
  FALSE_POSITIVE = { "String" => %w[pack] }.freeze

  # Which of the shared manual arrays sp_poly_responds_builtin already ORs
  # in alongside each class's generated array -- uni[] unconditionally for
  # every class (checked once, before any class dispatch), enumm[]
  # additionally for Array and Hash, numm[] additionally for Integer and
  # Float. A name already covered by one of these must not be duplicated
  # into the generated array -- the hand-written tables this replaces
  # never did, and generating it anyway would only make the output bigger
  # without changing any respond_to? answer.
  SHARED_ARRAYS = {
    "Array" => %w[uni enumm], "Hash" => %w[uni enumm], "String" => %w[uni],
    "Integer" => %w[uni numm], "Float" => %w[uni numm],
  }.freeze

  # bop_rows families that dispatch to Range's cn=="Range" arm at runtime
  # (TY_RANGE, TY_FLOAT_RANGE, TY_STR_RANGE -- three BuiltinRowGen families
  # because the sample receivers differ, but one C-level class name, so a
  # row registered under any of the three must be covered the same way).
  RANGE_FAMILIES = ["Range", "Range(Float)", "Range(String)"].freeze

  # bop_rows registers these under TY_FLOAT_RANGE/TY_STR_RANGE as
  # raise-stubs (sp_frange_iter_raise/equivalent) so a float/string Range
  # gives a sensible error if called with an Array-shaped method instead
  # of silently falling through -- not real Range methods: CRuby
  # (1..5).respond_to?(:reverse) is false, same shape as String's "pack"
  # false positive above.
  RANGE_FALSE_POSITIVE = %w[reverse join index].freeze

  # BuiltinRowGen::FAMILIES attributes every TY_POLY row to String (and
  # Symbol), same misattribution FALSE_POSITIVE["String"] above excludes
  # "pack" from -- "dump"/"undump" and the other boxed String transforms
  # below are real String methods, not Symbol's (CRuby:
  # :sym.respond_to?(:dump) is false), and "pack" isn't a String or Symbol
  # method at all (it's Array#pack's row).
  SYMBOL_FALSE_POSITIVE = %w[dump undump pack b scrub encode strip lstrip rstrip
                             chomp chop to_str].freeze

  class GenError < StandardError; end

  module_function

  # rt_header_text: lib/spinel_rt.h's current text, read once by the
  # caller. uni[]/enumm[]/numm[]/procm[]/rngm[]/symm[]/excm[] are each read
  # out of it once here too, not once per family that references them.
  def generate(rt_header_text)
    shared_arrays = SHARED_ARRAYS.values.flatten.uniq.to_h { |a| [a, array_literal_names(rt_header_text, a)] }
    procm = array_literal_names(rt_header_text, "procm")
    rngm = array_literal_names(rt_header_text, "rngm")
    symm = array_literal_names(rt_header_text, "symm")
    excm = array_literal_names(rt_header_text, "excm")
    uni = shared_arrays.fetch("uni")
    check_proc_coverage(uni, procm)
    check_range_coverage(uni, shared_arrays.fetch("enumm"), rngm)
    check_symbol_coverage(uni, symm)
    check_exception_coverage(uni, excm, exception_gated_names(rt_header_text))

    names_by_family = FAMILIES.to_h { |f| [f, []] }
    BuiltinRowGen.method_list.each do |family, name|
      next unless names_by_family.key?(family)
      next if BuiltinRowGen::INTERNAL_NAME.match?(name)
      next if FALSE_POSITIVE.fetch(family, []).include?(name)
      next if SHARED_ARRAYS.fetch(family).any? { |a| shared_arrays.fetch(a).include?(name) }
      names_by_family[family] << name
    end
    names_by_family.each_value { |names| names.sort!.uniq! }
    render(names_by_family)
  end

  # Every TY_PROC row bop_rows registers, minus the ones the universal
  # uni[] array already covers, must already be a literal in procm[]'s
  # text -- otherwise bop_rows grew a Proc method procm[] doesn't know
  # about, and a human needs to add it by hand (see the spec's Scope
  # section: Proc is verified-complete, not generated).
  def check_proc_coverage(uni, procm)
    proc_rows = BuiltinRowGen.method_list
                             .select { |family, _| family == "Proc" }
                             .map(&:last)
                             .reject { |name| BuiltinRowGen::INTERNAL_NAME.match?(name) }
                             .uniq
    missing = proc_rows - uni - procm
    return if missing.empty?

    raise GenError,
          "bop_rows now has Proc method(s) procm[]/uni[] in lib/spinel_rt.h " \
          "don't cover: #{missing.join(", ")}. Add them to procm[] by hand " \
          "(Proc is verified-complete, not generated -- see the spec)."
  end

  # Every bop_rows row under any RANGE_FAMILIES family, minus what uni[]/
  # enumm[] already cover, must already be a literal in rngm[]'s text --
  # otherwise bop_rows grew a Range method rngm[] doesn't know about. Range
  # itself is not generated (most of its real surface is desugared through
  # Enumerable rather than a bop_rows row, so a generator reading only
  # bop_rows would regenerate the same under-complete table #8153 fixed);
  # this only re-checks the part of its surface bop_rows *does* know about.
  def check_range_coverage(uni, enumm, rngm)
    range_rows = BuiltinRowGen.method_list
                              .select { |family, _| RANGE_FAMILIES.include?(family) }
                              .map(&:last)
                              .reject { |name| BuiltinRowGen::INTERNAL_NAME.match?(name) }
                              .uniq
    missing = range_rows - uni - enumm - rngm - RANGE_FALSE_POSITIVE
    return if missing.empty?

    raise GenError,
          "bop_rows now has Range method(s) rngm[]/enumm[]/uni[] in lib/spinel_rt.h " \
          "don't cover: #{missing.join(", ")}. Add them to rngm[] by hand " \
          "(Range is not generated -- see the spec)."
  end

  # Every bop_rows row under the Symbol family, minus what uni[] already
  # covers, must already be a literal in symm[]'s text. Symbol is not
  # generated for the same reason as Range (desugared Enumerable surface);
  # this only re-checks bop_rows's own slice of it.
  def check_symbol_coverage(uni, symm)
    symbol_rows = BuiltinRowGen.method_list
                               .select { |family, _| family == "Symbol" }
                               .map(&:last)
                               .reject { |name| BuiltinRowGen::INTERNAL_NAME.match?(name) }
                               .uniq
    missing = symbol_rows - uni - symm - SYMBOL_FALSE_POSITIVE
    return if missing.empty?

    raise GenError,
          "bop_rows now has Symbol method(s) symm[]/uni[] in lib/spinel_rt.h " \
          "don't cover: #{missing.join(", ")}. Add them to symm[] by hand " \
          "(Symbol is not generated -- see the spec)."
  end

  # Every bop_rows row under the Exception family, minus what uni[]/excm[]
  # already cover, must either be in excm[] or be one of the subclass-
  # gating if-chain's literal names (exception_gated_names). bop_rows has
  # no owning-subclass field for these (key, receiver, errno, ...), so
  # sp_poly_responds_builtin gates each by hand with sp_exc_is_a -- not an
  # array literal, hence the separate scan instead of array_literal_names.
  def check_exception_coverage(uni, excm, gated)
    exc_rows = BuiltinRowGen.method_list
                            .select { |family, _| family == "Exception" }
                            .map(&:last)
                            .reject { |name| BuiltinRowGen::INTERNAL_NAME.match?(name) }
                            .uniq
    missing = exc_rows - uni - excm - gated
    return if missing.empty?

    raise GenError,
          "bop_rows now has Exception method(s) excm[]/the subclass-gating " \
          "if-chain/uni[] in lib/spinel_rt.h don't cover: #{missing.join(", ")}. " \
          "Add them to excm[] (if every exception has it) or the if-chain " \
          "(if only one subclass does) by hand -- Exception is not generated; " \
          "see the spec."
  end

  # The strcmp(m, "...") literals inside sp_poly_responds_builtin's
  # Exception arm, bounded by its SP_BUILTIN_EXCEPTION check and its
  # trailing `return 0;` -- the per-subclass accessor gate bop_rows cannot
  # express itself, so it is a C if-chain rather than an array literal.
  def exception_gated_names(text)
    start = text.index("v.cls_id == SP_BUILTIN_EXCEPTION") or
      raise GenError, "no Exception arm found in lib/spinel_rt.h (sp_poly_responds_builtin changed shape?)"
    stop = text.index(/return 0;\s*\}/, start) or
      raise GenError, "Exception arm in lib/spinel_rt.h has no closing `return 0; }`"
    text[start...stop].scan(/strcmp\(m, "([^"]+)"\)/).flatten.uniq
  end

  # The string literals inside `static const char *const <array_name>[] =
  # { ... };` in lib/spinel_rt.h's text, by direct string scanning (the
  # same "read the hand-kept array as text" approach builtin_row_gen.rb
  # itself already uses on builtin_ops.c).
  def array_literal_names(text, array_name)
    start = text.index(/static const char \*const #{array_name}\[\] = \{/) or
      raise GenError, "no #{array_name}[] array found in lib/spinel_rt.h"
    stop = text.index("};", start) or
      raise GenError, "#{array_name}[] array in lib/spinel_rt.h has no closing };"
    text[start...stop].scan(/"((?:[^"\\]|\\.)*)"/).flatten.map { |s| s.gsub(/\\(.)/, '\1') }
  end

  def render(names_by_family)
    out = +"/* Generated by tools/gen_respond_to_tables.rb from src/builtin_ops.c's\n" \
          " * bop_rows[]. Do not edit by hand -- edit bop_rows[] and regenerate.\n" \
          " * See this script's own comments for the design rationale.\n" \
          " */\n"
    FAMILIES.each do |family|
      const = "sp_gen_responds_#{family.downcase}"
      out << "static const char *const #{const}[] = {\n"
      names_by_family.fetch(family).each { |n| out << "  #{n.inspect},\n" }
      out << "  NULL\n};\n"
    end
    out
  end
end

if $PROGRAM_NAME == __FILE__
  rt_header = File.read(File.join(GenRespondToTables::ROOT, "lib/spinel_rt.h"))
  puts GenRespondToTables.generate(rt_header)
end
