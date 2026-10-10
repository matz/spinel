# spinel: share
# Loading a native class must not make a fresh user return look hookable.
require "stringio"
io = StringIO.new("loaded native class")
raise "StringIO did not load" unless io.read == "loaded native class"

class Params
  def freshness_probe_token
    String.new("fresh")
  end
end

receiver = [Params.new, Object.new][ARGV.length]
token = receiver.freshness_probe_token
saved = token
token << "-after"
puts saved
