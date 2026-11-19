# JSON::ParserError#line and #column: the line and column the message names,
# as json 2.18.0 (Ruby 4.0.9) records them. nil on an error the program makes
# itself and on a NestingError; NoMethodError on another exception class. A
# class of the program's with methods of those names keeps its own, also
# when the error is rescued under a class above it, called with a block, or
# when another class makes the name private.
require "json"

def err(text, **options)
  JSON.parse(text, **options)
  nil
rescue JSON::ParserError => e
  e
end

["[1 2]", "\n\n  [1 2]", "", "[\"\\q\"]", "{\"a\" 1}", "[\"a\nb\"]", "nul", "[1,]",
 "  \n /*x", "[1] x"].each do |text|
  e = err(text)
  p [text, e.line, e.column]
end
e = err("{\"a\":1,\"a\":2}", allow_duplicate_key: false)
p [e.line, e.column]
e = err("[[1]]", max_nesting: 1)
p [e.class, e.line, e.column]

made = JSON::ParserError.new("made")
p [made.line, made.column]
nesting = JSON::NestingError.new("nesting")
p [nesting.line, nesting.column]
p made.respond_to?(:line), made.respond_to?(:column)
p StandardError.new.respond_to?(:line), KeyError.new("k").respond_to?(:column)
begin
  StandardError.new("s").line
rescue NoMethodError => error
  puts error.message
end

begin
  JSON.parse("[1 2]")
rescue => error
  p [error.line, error.column, error.line + 1, "at #{error.line}:#{error.column}"]
end
union = [1, err("[x]")].last
p [union.line, union.column]
errors = ["[", "{", "[1 2"].map { |text| err(text) }
p errors.map(&:line), errors.map { |error| error.column }

class Sub < JSON::ParserError; end
p [Sub.new("sub").line, Sub.new("sub").column]
begin
  raise Sub, "raised"
rescue JSON::ParserError => error
  p [error.class, error.line]
end

class OwnError < StandardError
  attr_reader :line
  def initialize(msg, line)
    super(msg)
    @line = line
  end

  def column = 99
end
begin
  raise OwnError.new("own", 7)
rescue OwnError => error
  p [error.line, error.column]
end
class Place
  def line = :place_line
  def column = :place_column
end
p [Place.new.line, Place.new.column]
# another class's private #line leaves OwnError's public one reachable
class QuietError < StandardError
  private

  def line = :quiet_line
end
begin
  raise OwnError.new("own", 9)
rescue StandardError => error
  p [error.line, error.column, error.respond_to?(:line), error.send(:line)]
  p [error.line { 1 }, error.column { 2 }]
end
[-> { raise OwnError.new("own", 7) }, -> { JSON.parse("[1 2]") }, -> { raise Sub, "sub" }].each do |block|
  block.call
rescue OwnError, JSON::ParserError => error
  p [error.class, error.line, error.column, error.line { 1 }, error.column { 2 }]
end
begin
  raise QuietError, "quiet"
rescue => error
  p error.send(:line)
end
class OwnSub < JSON::ParserError
  def line = :own_line
end
begin
  raise OwnSub, "x"
rescue JSON::ParserError => error
  p [error.line, error.column]
end
class OwnKeyError < StandardError
  def key = :own_key
end
begin
  {}.fetch(:missing)
rescue => error
  p [error.key, error.key { 1 }]
end
