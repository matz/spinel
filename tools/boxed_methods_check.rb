#!/usr/bin/env ruby
#
# Boxed-method drift check: every method name src/builtin_ops.c's bop_rows
# registers for IO, Thread, Mutex, ConditionVariable and Queue must be in
# the respond_to? lists a BOXED receiver of that class answers from
# (lib/spinel_rt.h: uni[] for each class, and for an IO the family lists
# of sp_io_responds that a boxed handle reads), or in
# KNOWN_GAPS below. A name the typed path calls and the boxed path does
# not is the shape of boxcar items 11 and 205 (an IO read back out of an
# Array raised NoMethodError for getbyte and each_line).
#
#   ruby tools/boxed_methods_check.rb
#
# KNOWN_GAPS only shrinks: a name listed there that the boxed lists now
# hold fails the check too, so a fix removes its entry in the same commit.
#
# Exit status: 0 no drift, 1 drift, 4 the tool's own error.
Encoding.default_external = Encoding::UTF_8

module BoxedMethodsCheck
  ROOT = File.expand_path("..", __dir__)
  KINDS = { "IO" => "IO", "THREAD" => "Thread", "MUTEX" => "Mutex",
            "CONDVAR" => "ConditionVariable", "QUEUE" => "Queue" }.freeze
  SHARED_LISTS = %w[uni].freeze
  # sp_io_responds's lists for a boxed handle: the names both faces call and
  # the boxed-only names (the *_t lists are the typed-only names)
  # (consolem: io/console's names, in a program that requires it)
  IO_LISTS = %w[iom iom_b statm statm_b filem filem_b basicm basicm_b ipm ipm_b
                udpm udpm_b unixm unixm_b serverm serverm_b socketm socketm_b consolem].freeze
  CLASS_LISTS = { "IO" => IO_LISTS }.freeze

  KNOWN_GAPS = {
    # typed-only IO names (sp_io_responds's iom_t, filem_t, statm_t)
    "IO" => %w[timeout timeout= ftype lineno= chmod chown close_on_exec= autoclose= wait
               reopen lstat pid mtime atime ctime birthtime] +
            # Enumerable names a typed IO calls through its rows; enumm[] holds
            # them for Array, Hash, Range and Enumerator only
            %w[chain tally zip chunk collect each_with_object grep grep_v inject map
               reduce to_h to_set],
    # Thread, Mutex, ConditionVariable and Queue have no boxed respond_to?
    # list yet (boxcar items 50, 67 and 29)
    "Thread" => %w[value join kill exit terminate raise wakeup run alive? stop?
                   report_on_exception report_on_exception= status name name= priority
                   priority= [] []= key? thread_variable_get thread_variable_set
                   thread_variable? keys],
    "Mutex" => %w[lock unlock try_lock locked? owned? mon_owned? synchronize sleep],
    "ConditionVariable" => %w[wait signal broadcast],
    "Queue" => %w[pop shift deq push << enq close clear length num_waiting empty? closed?
                  size max],
  }.freeze

  module_function

  def row_names(ops)
    names = Hash.new { |h, k| h[k] = [] }
    ops.scan(/\{ TY_(#{KINDS.keys.join("|")}), *"([^"]+)"/) do |kind, name|
      names[KINDS.fetch(kind)] << name unless name.start_with?("__")
    end
    names.transform_values(&:uniq)
  end

  def c_list(rt, name)
    body = rt[/static const char \*const #{Regexp.escape(name)}\[\] = \{(.*?)NULL \};/m, 1]
    body ? body.scan(/"([^"]+)"/).flatten : []
  end

  def boxed_names(rt, cls)
    (SHARED_LISTS + CLASS_LISTS.fetch(cls, [])).flat_map { |n| c_list(rt, n) }
  end

  def problems(ops, rt, gaps = KNOWN_GAPS)
    out = []
    names = row_names(ops)
    KINDS.each_value do |cls|
      boxed = boxed_names(rt, cls)
      known = gaps.fetch(cls, [])
      rows = names.fetch(cls, [])
      (rows - boxed - known).each { |n| out << "#{cls}##{n}: a typed row, in no boxed list and not in KNOWN_GAPS" }
      (known & boxed).each { |n| out << "#{cls}##{n}: in KNOWN_GAPS and in a boxed list; remove it from KNOWN_GAPS" }
      (known - rows).each { |n| out << "#{cls}##{n}: in KNOWN_GAPS and in no row" }
    end
    out
  end

  def run
    ops = File.read(File.join(ROOT, "src/builtin_ops.c"))
    rt = File.read(File.join(ROOT, "lib/spinel_rt.h"))
    raise "no IO rows found in src/builtin_ops.c" if row_names(ops).fetch("IO", []).empty?
    raise "no iom[] list found in lib/spinel_rt.h" if c_list(rt, "iom").empty?
    found = problems(ops, rt)
    found.each { |line| warn "boxed-methods-check: #{line}" }
    puts "boxed-methods-check: #{found.empty? ? "OK" : "#{found.size} problem(s)"}"
    found.empty? ? 0 : 1
  rescue StandardError => e
    warn "boxed-methods-check: #{e.message}"
    4
  end
end

exit BoxedMethodsCheck.run if $PROGRAM_NAME == __FILE__
