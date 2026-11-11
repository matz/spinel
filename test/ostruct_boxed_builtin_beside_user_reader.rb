# A boxed call whose name a class of the program reads (path) and File#path
# or Dir#path answers too, with ostruct required: an OpenStruct member makes
# the call's slot boxed, and the builtin arm boxes its String into it. It
# assigned the raw C string, and the C did not build.
require "ostruct"

class Decoder
  attr_reader :path
  def initialize(path) = @path = path
end

cache = { "k" => Decoder.new("lib.so") }
puts cache["k"].path
p OpenStruct.new(path: "member").path
file = File.open(__FILE__) { |f| f }
[cache["k"], file, OpenStruct.new(path: 3)].each { |x| p(x.path == __FILE__ ? :this_file : x.path) }
[cache["k"], Dir.new("."), OpenStruct.new(path: 4)].each { |x| p x.path }

# to_path, which File and Dir answer too, beside a class's reader of it
class Source
  attr_reader :to_path
  def initialize(to_path) = @to_path = to_path
end
sources = { "k" => Source.new("src.rb") }
[sources["k"], file, Dir.new("."), OpenStruct.new(to_path: 5)].each { |x| p(x.to_path == __FILE__ ? :this_file : x.to_path) }
