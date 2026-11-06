# `&:m` over a receiver the class-id dispatch may serve is viewed as a
# rest-parameter block; the `_spx` it replaced must not stay a block local.
class Pair
  def initialize(a, b)
    @a = a
    @b = b
  end

  def map
    [yield(@a)]
  end

  def each
    yield "pair", [@a, @b]
  end
end

def show(headers)
  out = []
  headers.each do |name, value|
    values = value.is_a?(Array) ? value.map(&:to_s) : [value.to_s]
    out << "#{name}=#{values.join(",")}"
  end
  out
end

[{ "a" => ["x", 1], "c" => 3 }, Pair.new(4, :e)].each { |h| p show(h) }
