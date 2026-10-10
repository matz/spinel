# A same-named but unreachable user method stays in the program while the
# String receiver's sub result is used. The unrelated Thread.value mutation
# must still see the shared String, and a non-matching sub must remain fresh.
# spinel: share
module Params
  def self.sub(a, b)
    nil
  end
end

protocol = Thread.new { "https://" }.value.sub("://", "")
trigger = String.new("trigger")
Thread.new { trigger }.value << "-after"

no_match_source = Thread.new { String.new("plain") }.value
no_match = no_match_source.sub("://", "!")
no_match << "!"

puts protocol
puts trigger
puts no_match_source
puts no_match
