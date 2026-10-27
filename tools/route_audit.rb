#!/usr/bin/env ruby
# frozen_string_literal: true
# route_audit.rb -- every target of a call goes through the call plan.
#
#   tools/route_audit.rb [FILE...]
#
# call_target (call_plan.c) is the one place a site asks which method a
# call runs, so that a call bound to one of a method's versions (#9898) is
# bound everywhere. In the files tools/route_audit.allow lists under
# `files:`, a by-name lookup of the tables (comp_method_in_chain and kin)
# passes when its name is a string literal (a method the language calls by
# a fixed name) or when its function (or the function and that lookup) is
# listed there with the reason it asks about a name rather than one call. Any other is a call target that
# bypasses call_target. With FILEs, only those of them the list names are
# read (gate.rb passes a commit's changed sources).
#
# Exit status: 0 clean, 1 a lookup to route or list.
ROOT = File.expand_path("..", __dir__)
LOOKUP = /\b(comp_method_in_chain|comp_cmethod_in_chain|comp_method_in_class|comp_cmethod_in_class|comp_method_index|comp_name_responders)\(/
NAME_ARG = { "comp_method_index" => 1, "comp_name_responders" => 1 }.freeze

allow = {}
files = []
File.foreach(File.join(ROOT, "tools/route_audit.allow")) do |l|
  l = l.sub(/#.*/, "").strip
  next if l.empty?
  if l.start_with?("files:") then files.concat(l.split[1..])
  else
    f, fn, look, = l.split(/\s+/, 4)
    allow[look&.start_with?("comp_") ? "#{f} #{fn} #{look}" : "#{f} #{fn}"] = true
  end
end
files &= ARGV.map { |a| a.delete_prefix("#{ROOT}/") } unless ARGV.empty?

# the arguments of the call whose "(" ends at index i of line s
def args(s, i)
  depth = 1
  out = +""
  parts = []
  s[i..].each_char do |ch|
    depth += 1 if ch == "("
    depth -= 1 if ch == ")"
    break if depth.zero?
    if ch == "," && depth == 1 then parts << out.strip; out = +"" else out << ch end
  end
  parts << out.strip
end

bad = []
files.each do |f|
  fn = nil
  File.readlines(File.join(ROOT, f), encoding: "UTF-8").each_with_index do |l, n|
    fn = $1 if l =~ /\A[A-Za-z][^;=]*?\b(\w+)\(/ && !l.rstrip.end_with?(";")
    next if l.lstrip.start_with?("/*", "*")
    l.to_enum(:scan, LOOKUP).each do
      m = Regexp.last_match
      next if m[1] == fn   # the lookup's own definition
      name = args(l, m.end(0))[NAME_ARG.fetch(m[1], 2)].to_s
      next if name.start_with?('"') || fn == "call_target" || allow["#{f} #{fn}"] || allow["#{f} #{fn} #{m[1]}"]
      bad << "#{f}:#{n + 1}: #{fn}: #{m[1]}(..., #{name}, ...): a call's target goes through call_target, or list #{fn} in tools/route_audit.allow with why it asks about a name"
    end
  end
end
bad.each { |b| warn "route-audit: #{b}" }
exit(bad.empty? ? 0 : 1)
