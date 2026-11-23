# spinel: not-cruby -- verifies --strict-rbs compile-time refusal diagnostics
# spinel: rbs-seed-check
# Under --strict-rbs, RBS seed contradictions that default --rbs either drops/
# widens or turns into a runtime TypeError trap are refused at compile time
# with the Ruby file:line.
class StrictBadFoo; end
class StrictBadBar; end

class StrictBadParent
  def assign(row)
    @id = row["id"]
  end
end

class StrictBadChild < StrictBadParent
  def id = @id
end

class StrictBadBox
  def initialize(obj)
    @obj = obj
  end
end

class StrictBadApi
  def initialize(h)
    @headers = h
  end

  def send_headers(h)
    h["k"] || ""
  end

  def mixed_return
    { "k" => "v", "n" => 1 }
  end

  def set_bad_ivar
    @headers = { "k" => 1 }
  end

  #: (Hash[String, String]) -> String
  def inline_send(h)
    h["k"] || ""
  end
end

s = +"k"
s << "1"
u = [s, 1][0]
boxed_h = [{ "k" => "v" }, 1][0]

api = StrictBadApi.new({ u => "v" })
puts api.send_headers({ "k" => 1 })
puts api.send_headers({ "k" => "v", "n" => 1 })
puts api.send_headers({ u => "v" })
puts api.send_headers(boxed_h)
puts api.mixed_return["k"]
api.set_bad_ivar
puts api.inline_send({ "k" => 2 })
StrictBadBox.new(StrictBadBar.new)
c = StrictBadChild.new
c.assign({ "id" => 1, "name" => "x" })
p c.id
