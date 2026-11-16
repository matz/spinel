# spinel: gc-stress

require "json"

class Decoder
  def decode(bytes)
    value = JSON.parse(bytes)
    { "entry" => value, "bytes" => bytes, "findings" => [] }
  end
end

class Boundary
  def initialize
    @decoder = Decoder.new
  end
  def attempt(scope)
    raise ArgumentError unless scope.is_a?(Hash)
    keys = scope.transform_keys(&:to_sym)
    { "findings" => [] }.merge(yield(keys))
  rescue ArgumentError, TypeError, KeyError
    { "outcome" => "invalid", "findings" => [] }
  rescue IOError => error
    { "outcome" => "unknown", "findings" => [], "error" => error }
  end
  def read(scope:, bytes:)
    attempt(scope) do |keys|
      { "value" => @decoder.decode(bytes), "scope" => keys }
    end
  end
end

boundary = Boundary.new
scope = { account: "A", deployment: "fixture" }
puts boundary.read(scope: scope, bytes: '{"n":1}').fetch("value").fetch("entry").fetch("n")

def wrap(keys)
  { "findings" => [] }.merge(yield(keys))
end
puts wrap(:read) { |keys| { "entry" => 1 } }.fetch("entry")
puts wrap(:write) { |keys| { "outcome" => "written" } }.fetch("outcome")
puts wrap(:early) { |keys| next { "outcome" => "stale" }; { "entry" => 2 } }.fetch("outcome")
