# spinel: needs-rbs-parser
class Sink
  #: (Hash[String, untyped], ?Array[String]) -> Hash[String, untyped]
  def accept(payload, suffix = [])
    suffix << "seen"
    payload["seen"] = suffix.size
    payload
  end
end

class Other
end

def pick(n) = n == 1 ? Sink.new : Other.new

p Sink.new.accept({ "type" => "seed", "count" => 0 })
source = { "type" => "spread", "count" => 1 }
p pick(1).accept(**source)
p source
