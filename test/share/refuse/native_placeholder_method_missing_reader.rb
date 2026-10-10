# spinel: not-cruby
# Keep native-class reader declarations among conservative hook candidates.
require "stringio"
StringIO.new("loaded native class").read
class StringIO
  attr_reader :method_missing
end

class NativePlaceholderReaderFreshArm
  def freshness_probe_token = String.new("fresh")
end
receiver = [NativePlaceholderReaderFreshArm.new, Object.new][ARGV.length]
token = receiver.freshness_probe_token
saved = token
token << "-after"
puts saved
