#!/usr/bin/env ruby
# frozen_string_literal: true

# Generate src/implied_calls.inc, the rows of nt_implied_calls (src/node_table.c),
# from tools/implied_calls.yml: for each node kind the compiler's node table
# has, the methods CRuby calls for it that the program does not name.
#
#   ruby tools/gen_implied_calls.rb            # print the file
#   ruby tools/gen_implied_calls.rb --write    # regenerate src/implied_calls.inc
#   ruby tools/gen_implied_calls.rb --check    # fail when a node kind has no
#                                              # entry, a row is malformed, or
#                                              # the generated file is stale
#                                              # (make implied-calls-check)
#
# The generated file is checked in, so a build needs no Ruby (the way
# src/value_pos_arms.inc is). --check reads src/node_kinds.inc for the kinds,
# and, when the pinned Prism gem's vendor/prism/config.yml is there (PRISM_CONFIG
# names another; `make deps` fetches it), holds every field a row names against
# the kind's fields in it.
#
# What it emits: the file is included three times, with NTI_PART set to
#   1  one row per implied call, sorted by node kind (the order of the NodeKind
#      enum: nt_implied_calls searches it by kind), in the order of the entry
#   2  a `case NK_<kind>:` for each kind of `literal`
#   3  a `case NK_<kind>:` for each kind of `not_value`
# A row of a name under `conversions` carries that name's class as `.target`.
# A row's `planned: true` (its reader asks the call plan) is emitted as
# `.planned = 1` on its row. The key is temporary: each reader's move onto the
# plan sets it for that reader's rows, and once every row has it the key, this
# field and the check on it are deleted (#10866, step 2's last implied change).

require "yaml"
require "optparse"

ROOT = File.expand_path("..", __dir__)
CALLS = File.join(ROOT, "tools/implied_calls.yml")
KINDS = File.join(ROOT, "src/node_kinds.inc")
CONFIG = ENV["PRISM_CONFIG"] || File.join(ROOT, "vendor/prism/config.yml")
OUTPUT = File.join(ROOT, "src/implied_calls.inc")

SIDES = { "inst" => "NTS_INST", "class" => "NTS_CLASS", "both" => "NTS_INST | NTS_CLASS" }.freeze
CONDS = { nil => "NTC_NONE", "nonliteral" => "NTC_NONLITERAL", "no_else" => "NTC_NO_ELSE",
          "multi_target" => "NTC_MULTI_TARGET", "gvar_port" => "NTC_GVAR_PORT" }.freeze
ABSENTS = { nil => "NTA_SKIP", "skip" => "NTA_SKIP", "self" => "NTA_SELF", "none" => "NTA_NONE" }.freeze
# The operands that no child field holds
WORDS = { "node" => "NTO_NODE", "self" => "NTO_SELF", "yielded" => "NTO_YIELDED",
          "matched" => "NTO_MATCHED", "none" => "NTO_NONE" }.freeze
MAX_STEPS = 3
# The kinds whose Prism `read_name` the node table keeps as `name` (spinel_parse.c)
RENAMED_NAME = %w[CallOperatorWriteNode CallOrWriteNode CallAndWriteNode].freeze

Step = Struct.new(:field, :kind, :list, :chain)
Row = Struct.new(:kind, :name, :name_field, :operand, :steps, :side, :cond, :absent, :planned, :target, :text)
# The builtin classes a conversion may name (lib/sp_builtin_classes.h)
BUILTIN_CLASSES = File.join(ROOT, "lib", "sp_builtin_classes.h")

# YAML.safe_load_file keeps the last of a repeated key, so a node written twice
# would pass for written once. Walk the parse tree and name every mapping key
# that repeats in its mapping, with its line.
def repeated_keys(node, found = [])
  if node.is_a?(Psych::Nodes::Mapping)
    seen = {}
    node.children.each_slice(2) do |k, _|
      next unless k.is_a?(Psych::Nodes::Scalar)
      found << "#{k.value} (line #{k.start_line + 1}, first at line #{seen[k.value] + 1})" if seen.key?(k.value)
      seen[k.value] ||= k.start_line
    end
  end
  (node.children || []).each { |c| repeated_keys(c, found) }
  found
end

def node_kinds
  File.read(KINDS).scan(/^\s+X\((\w+)\)/).flatten
end

# `value[CallNode].receiver` -> steps; `conditions.*` -> a list step
def parse_steps(text, errors, where)
  steps = []
  text.split(".").each do |part|
    if part == "*"
      if steps.empty? || steps.last.list
        errors << "#{where}: `*` must follow a field"
      else
        steps.last.list = true
      end
    elsif part =~ /\A(\w+)(?:\[(\w+)\])?(\^)?\z/
      steps << Step.new($1, $2, false, !$3.nil?)
      errors << "#{where}: ^ needs a kind in brackets, and is for the last step" if $3 && ($2.nil? || part != text.split(".").last)
    else
      errors << "#{where}: bad operand step #{part.inspect}"
    end
  end
  errors << "#{where}: an operand has at most #{MAX_STEPS} steps" if steps.size > MAX_STEPS
  steps
end

def load_rows(errors)
  tree = YAML.parse_file(CALLS)
  dups = tree ? repeated_keys(tree) : []
  errors << "#{CALLS}: repeated keys: #{dups.join(", ")}" unless dups.empty?
  y = YAML.safe_load_file(CALLS)
  abort "#{CALLS}: no `nodes` map" unless y.is_a?(Hash) && y["nodes"].is_a?(Hash)
  rows = []
  y["nodes"].each do |kind, e|
    calls = e.is_a?(Hash) ? e["calls"] : nil
    (errors << "#{kind}: needs `calls`, a list of rows" and next) unless calls.is_a?(Array) && !calls.empty?
    calls.each_with_index { |c, i| rows << make_row(kind, c, "#{kind} row #{i + 1}", errors) }
  end
  conv = y["conversions"] || {}
  (errors << "#{CALLS}: `conversions` is a map of a name to a class" and conv = {}) unless conv.is_a?(Hash)
  classes = File.exist?(BUILTIN_CLASSES) ? File.read(BUILTIN_CLASSES).scan(/X\(\w+, "([^"]+)"/).flatten : []
  conv.each do |name, cls|
    errors << "conversions: #{name} names no row" unless rows.any? { |r| r.name == name }
    errors << "conversions: #{name}: #{cls.inspect} is not a builtin class (#{BUILTIN_CLASSES})" unless classes.include?(cls)
  end
  rows.each { |r| r.target = conv[r.name] }
  [rows, y["literal"] || [], y["not_value"] || [], y["none"] || [], y["super"] || []]
end

def make_row(kind, c, where, errors)
  extra = c.keys - %w[name name_field operand side when absent planned]
  errors << "#{where}: unknown keys #{extra.join(", ")}" unless extra.empty?
  errors << "#{where}: needs exactly one of `name` and `name_field`" unless c.key?("name") ^ c.key?("name_field")
  errors << "#{where}: unknown side #{c["side"].inspect}" unless SIDES.key?(c["side"])
  errors << "#{where}: unknown condition #{c["when"].inspect}" unless CONDS.key?(c["when"])
  errors << "#{where}: unknown absent #{c["absent"].inspect}" unless ABSENTS.key?(c["absent"])
  errors << "#{where}: `planned` is true or absent" unless [nil, true].include?(c["planned"])
  operand = c["operand"].to_s
  steps = []
  if WORDS.key?(operand)
    errors << "#{where}: `absent` is for an operand field" if c["absent"]
  else
    steps = parse_steps(operand, errors, where)
    errors << "#{where}: no operand" if steps.empty?
  end
  Row.new(kind, c["name"], c["name_field"], operand, steps, c["side"], c["when"], c["absent"], c["planned"], nil, where)
end

# ---- the check against the kinds and Prism ----

def prism_fields(config)
  config["nodes"].to_h do |n|
    [n["name"], (n["fields"] || []).to_h { |f| [f["name"], { node: %w[node node? node[]].include?(f["type"]), list: f["type"] == "node[]",
                                                             kinds: Array(f["kind"]).grep(String) }] }]
  end
end

def check_row(row, prism, errors)
  fields = prism[row.kind]
  if row.name_field
    f = fields[row.name_field]
    f ||= fields["read_name"] if row.name_field == "name" && RENAMED_NAME.include?(row.kind)
    errors << "#{row.text}: name_field #{row.name_field} is not a field of #{row.kind}" unless f
    errors << "#{row.text}: name_field #{row.name_field} holds a node, not a name" if f && f[:node]
  end
  cur = fields
  row.steps.each_with_index do |step, i|
    f = cur && cur[step.field]
    unless f && f[:node]
      errors << "#{row.text}: #{step.field} is not a node field of #{i.zero? ? row.kind : "the step before"}"
      return
    end
    errors << "#{row.text}: #{step.field} is a list in Prism; write #{step.field}.*" if f[:list] && !step.list
    errors << "#{row.text}: #{step.field} is a single node in Prism, not a list" if !f[:list] && step.list
    # the next step's field must exist in every kind this one allows (or the guard)
    allowed = step.kind ? [step.kind] : f[:kinds]
    errors << "#{row.text}: #{step.kind} is not a node kind" if step.kind && !prism.key?(step.kind)
    cur = nil
    next_step = row.steps[i + 1]
    next unless next_step
    allowed.each do |k|
      errors << "#{row.text}: #{k} has no field #{next_step.field}" unless prism[k]&.key?(next_step.field)
    end
    cur = allowed.size == 1 ? prism[allowed.first] : nil
    break if cur.nil?
  end
end

def check(rows, literal, not_value, none, sup, errors)
  kinds = node_kinds
  errors << "#{KINDS} is missing or lists no kinds" if kinds.empty?
  with_calls = rows.map(&:kind).uniq
  (kinds - with_calls - none - sup).each { |k| errors << "#{k}: no entry in tools/implied_calls.yml (add its calls under `nodes`, or list it under `none` or `super`)" }
  (with_calls & none).each { |k| errors << "#{k}: under both `nodes` and `none`" }
  ((with_calls | none) & sup).each { |k| errors << "#{k}: under `super` and under `nodes` or `none`" }
  none.tally.each { |k, n| errors << "#{k}: listed #{n} times under `none`" if n > 1 }
  (with_calls + none + sup - kinds).uniq.each { |k| errors << "#{k}: in tools/implied_calls.yml, but not a node kind (src/node_kinds.inc)" }
  { "literal" => literal, "not_value" => not_value, "super" => sup }.each do |list, ks|
    ks.tally.each { |k, n| errors << "#{k}: listed #{n} times under `#{list}`" if n > 1 }
    (ks - kinds).each { |k| errors << "#{k}: under `#{list}`, but not a node kind" }
  end
  rows.each do |r|
    r.steps.each { |s| errors << "#{r.text}: guard #{s.kind} is not a node kind" if s.kind && !kinds.include?(s.kind) }
  end
  return unless File.exist?(CONFIG)
  prism = prism_fields(YAML.load_file(CONFIG))
  rows.each do |r|
    if prism.key?(r.kind) then check_row(r, prism, errors)
    else errors << "#{r.text}: Prism has no node #{r.kind}, so it has no fields to name"
    end
  end
end

# ---- the generated file ----

def c_string(s)
  s ? s.dump : "NULL"
end

def generate(rows, literal, not_value, sup)
  order = node_kinds.each_with_index.to_h
  out = +"/* Generated by tools/gen_implied_calls.rb from tools/implied_calls.yml; do not edit.\n"
  out << "   The calls CRuby makes for a node that the program does not name\n"
  out << "   (nt_implied_calls, src/node_table.c). Included five times, with NTI_PART set to\n"
  out << "     1  one row per call, sorted by node kind:\n"
  out << "        { kind, side, condition, absent, operand word, steps, name, name field, { steps } },\n"
  out << "        and .planned = 1 on a row whose reader asks the call plan (temporary, see the yml),\n"
  out << "        .target on a row of a conversion's name (the yml's `conversions`)\n"
  out << "     2  a case label for each kind that holds a scalar literal\n"
  out << "     3  a case label for each kind that is a pattern or a target, not a value\n"
out << "     4  a case label for each kind that is a super: the builtin of its def's name\n"
out << "     5  a case label for each kind that has rows */\n"
  out << "#if NTI_PART == 1\n"
  rows.sort_by.with_index { |r, i| [order.fetch(r.kind), i] }.each do |r|
    steps = r.steps.map { |s| "{ #{s.field.dump}, #{s.kind ? "NK_#{s.kind}" : 0}, #{s.list ? 1 : 0}, #{s.chain ? 1 : 0} }" }
    word = WORDS[r.operand] || "NTO_PATH"
    out << "  { NK_#{r.kind}, #{SIDES.fetch(r.side)}, #{CONDS.fetch(r.cond)}, #{ABSENTS.fetch(r.absent)}, #{word}, #{steps.size}, " \
           "#{c_string(r.name)}, #{c_string(r.name_field)}, { #{steps.empty? ? "{ NULL, 0, 0, 0 }" : steps.join(", ")} }" \
           "#{r.planned ? ", .planned = 1" : ""}#{r.target ? ", .target = #{c_string(r.target)}" : ""} },\n"
  end
  out << "#elif NTI_PART == 2\n"
  literal.sort_by { |k| order.fetch(k, 0) }.each { |k| out << "  case NK_#{k}:\n" }
  out << "#elif NTI_PART == 3\n"
  not_value.sort_by { |k| order.fetch(k, 0) }.each { |k| out << "  case NK_#{k}:\n" }
  out << "#elif NTI_PART == 4\n"
  sup.sort_by { |k| order.fetch(k, 0) }.each { |k| out << "  case NK_#{k}:\n" }
  out << "#elif NTI_PART == 5\n"
  rows.map(&:kind).uniq.sort_by { |k| order.fetch(k, 0) }.each { |k| out << "  case NK_#{k}:\n" }
  out << "#endif\n"
  out
end

opts = {}
OptionParser.new do |p|
  p.on("--write") { opts[:write] = true }
  p.on("--check") { opts[:check] = true }
end.parse!

errors = []
rows, literal, not_value, none, sup = load_rows(errors)
check(rows, literal, not_value, none, sup, errors)
if opts[:check]
  text = errors.empty? ? generate(rows, literal, not_value, sup) : nil
  errors << "src/implied_calls.inc is stale; regenerate with --write" if text && !(File.exist?(OUTPUT) && File.read(OUTPUT) == text)
  if errors.empty?
    kinds = rows.map(&:kind).uniq.size
    puts "implied-calls-check: ok (#{rows.size} calls of #{kinds} node kinds, #{none.size} kinds with none, #{sup.size} supers)"
  else
    warn errors.map { |m| "implied-calls-check: #{m}" }
    exit 1
  end
else
  abort errors.map { |m| "implied-calls: #{m}" }.join("\n") unless errors.empty?
  text = generate(rows, literal, not_value, sup)
  if opts[:write]
    File.write(OUTPUT, text)
    puts "wrote #{OUTPUT}"
  else
    print text
  end
end
