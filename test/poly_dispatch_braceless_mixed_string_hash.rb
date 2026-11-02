# spinel: needs-rbs-parser
class MixedSink
  #: (Hash[String, untyped]) -> Hash[String, untyped]
  def accept(payload)
    payload["received"] = true
    payload
  end

  #: (Hash[String, untyped]) -> Hash[String, untyped]
  def self.accept(payload)
    payload["received"] = true
    payload
  end
end

class OtherSink
end

def pick_sink(n)
  n == 1 ? MixedSink.new : OtherSink.new
end

def pick_sink_class(n)
  n == 1 ? MixedSink : OtherSink
end

p MixedSink.new.accept({ "type" => "seed", "count" => 1 })
p pick_sink(1).accept("type" => "instance", "count" => 2, "enabled" => false, "missing" => nil)

source = { "type" => "spread", "count" => 3, "enabled" => false, "missing" => nil }
p pick_sink(1).accept(**source)
raise "caller hash was changed" if source.key?("received")

p MixedSink.accept({ "type" => "seed class", "count" => 4 })
p pick_sink_class(1).accept("type" => "class", "count" => 5, "enabled" => false, "missing" => nil)
