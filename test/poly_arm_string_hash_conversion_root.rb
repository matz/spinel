# spinel: gc-stress
class Sink
  def accept(prefix = [], payload)
    prefix << "seen"
    payload["seen"] = prefix.size
    payload
  end

  def self.accept(payload, suffix = [])
    suffix << "seen"
    payload["seen"] = suffix.size
    payload
  end

  def keep(payload)
    mark = -> { payload["kept"] = true }
    mark.call
    payload
  end
end

class Other
end

def pick(n) = n == 1 ? Sink.new : Other.new
def pick_class(n) = n == 1 ? Sink : Other.new

p Sink.new.accept({ "type" => "seed", "count" => 0 })
p Sink.accept({ "type" => "seed class", "count" => 0 })
p Sink.new.keep({ "type" => "seed kept", "count" => 0 })
p pick(1).accept("type" => "instance", "count" => 1)
p pick_class(1).accept("type" => "class", "count" => 2)
p pick(1).keep("type" => "captured", "count" => 3)
