# Honest RBS contracts (external --rbs and inline RBS) compile cleanly and
# match CRuby under --strict-rbs, including subtype widening (Hash[String, String]
# into Hash[String, untyped], subclass into ancestor), empty container literals,
# and polymorphic dispatch where one arm takes a different type.
# spinel: rbs-seed-check
class StrictOkBase
  def tag = "base"
end

class StrictOkSub < StrictOkBase
  def tag = "sub"
end

class StrictOkStrArm
  #: (String) -> Integer
  def classify(x) = x.length
end

class StrictOkArrArm
  #: (Array[String]) -> Integer
  def classify(x) = x.length
end

class StrictOkApi
  # @rbs @headers: Hash[String, String]

  def initialize(headers)
    @headers = headers
  end

  def header(k)
    @headers[k] || ""
  end

  def copy_headers
    { "x" => @headers["x"] || "none" }
  end

  def empty_headers
    {}
  end

  def accept_any_str_hash(h)
    h.size
  end

  def take_base(obj)
    obj.tag
  end

  #: (Hash[String, String]) -> String
  def inline_header(h)
    h["k"] || ""
  end
end

empty = {}
api = StrictOkApi.new({ "x" => "ok" })
p api.header("x")
p api.copy_headers["x"]
p api.empty_headers.size
p api.accept_any_str_hash({ "a" => "b" })
p api.accept_any_str_hash({ "a" => 1 })
p api.take_base(StrictOkSub.new)
p api.inline_header({ "k" => "v" })
p api.inline_header(empty)

arms = [StrictOkStrArm.new, StrictOkArrArm.new]
p arms[0].classify("gear")
p arms[1].classify(["a", "b"])
