# spinel: not-cruby
# Keep the conservative refusal when a real inherited user hook exists.
require "stringio"
StringIO.new("loaded native class").read

class NativePlaceholderHookBase
  def method_missing(name, *args) = +"hook"
  def freshness_probe_token = String.new("fresh")
end
class NativePlaceholderHookChild < NativePlaceholderHookBase; end
class NativePlaceholderFreshArm
  def freshness_probe_token = String.new("fresh")
end

receiver = [NativePlaceholderFreshArm.new, NativePlaceholderHookChild.new][ARGV.length]
token = receiver.freshness_probe_token
saved = token
token << "-after"
puts saved
