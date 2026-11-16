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
  def append(scope:, bytes:, stale:)
    attempt(scope) do |keys|
      value = JSON.parse(bytes)
      outcome = stale ? "stale" : "written"
      next { "outcome" => outcome } if outcome == "stale"
      decoded = @decoder.decode(bytes)
      { "outcome" => outcome, "value" => decoded, "head" => value, "findings" => decoded.fetch("findings") }
    end
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
