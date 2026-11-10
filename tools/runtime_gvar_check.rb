#!/usr/bin/env ruby
# Holds the globals spinel's class facts treat as the runtime's own
# (is_runtime_gvar_name, src/builtin_names.c, and the punctuation ones) against
# the CRuby this runs under: every name in its global_variables must give a
# read the fact dump marks "any builtin", or a global the runtime sets would
# be taken for one nothing but the program writes (a send on it could then
# skip the builtin class that answers). The same for the constants the runtime
# defines that are no module (is_runtime_const_name) against its
# Object.constants.
#
#   SPINEL=bin/spinel ruby tools/runtime_gvar_check.rb      # make runtime-gvar-check
require "tmpdir"
require "open3"
require "rbconfig"

spinel = ENV.fetch("SPINEL", "bin/spinel")
bad = []
# Does the probe's receiver fact (nil for none) hold every builtin class? It does when its builtin part is any
# builtin, and when it is ANY, every class, which cls_fact_may_hold_builtin answers 1 for whatever the class: one
# walk alone (SPINEL_CLS_FLOW=unified) gives `$!` and `$@` that fact
def any_builtin?(fact) = fact && (fact.include?("+builtin[any]") || fact.chomp.end_with?(" receiver: ANY"))
Dir.mktmpdir("gvarcheck") do |dir|
  global_variables.map(&:to_s).each do |name|
    src = File.join(dir, "g.rb")
    File.write(src, "#{name}.gvar_probe\n")
    _out, err, _st = Open3.capture3({ "SPINEL_CLS_FACTS" => "1" }, spinel, src, "-c", "-o", File.join(dir, "g.c"))
    fact = err.lines.grep(/\Acls-fact: .* gvar_probe receiver:/).first
    bad << "#{name}: #{fact ? fact.sub(/.*receiver:/, 'receiver:').chomp : 'no fact'}" unless any_builtin?(fact)
  end
end
# the constants: a fresh Ruby, without the gems, so Object.constants is the runtime's
consts = IO.popen([RbConfig.ruby, "--disable-gems", "-e", "puts Object.constants.reject { |c| Object.const_get(c).is_a?(Module) }"], &:readlines).map(&:chomp)
bad << "no constants read from #{RbConfig.ruby}" if consts.empty?
Dir.mktmpdir("constcheck") do |dir|
  consts.each do |name|
    src = File.join(dir, "c.rb")
    File.write(src, "#{name}.gvar_probe\n")
    _out, err, _st = Open3.capture3({ "SPINEL_CLS_FACTS" => "1" }, spinel, src, "-c", "-o", File.join(dir, "c.c"))
    fact = err.lines.grep(/\Acls-fact: .* gvar_probe receiver:/).first
    bad << "#{name}: #{fact ? fact.sub(/.*receiver:/, 'receiver:').chomp : 'no fact'}" unless any_builtin?(fact)
  end
end
puts bad
exit(bad.empty? ? 0 : 1)
