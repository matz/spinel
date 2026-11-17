# A call on ENV through an expression that runs code to find it: a
# method that answers ENV and nothing else, called and used on the spot.
# The expression runs once, in its place (a short-circuit that skips the
# call skips it), and the call answers as on ENV: reads, writes that reach
# the environment, the enumeration, identity. It was refused ("hold ENV in
# a variable").
$n = 0
def e = ($n += 1; ENV)
class Cfg
  def initialize = @calls = 0
  attr_reader :calls
  def env = (@calls += 1; ENV)
end
ENV["SPX_W"] = "w"
c = Cfg.new
p c.env["SPX_W"], c.env.fetch("SPX_W", "d"), c.calls
p(false && e["SPX_W"])
p $n
p(true && e["SPX_W"])
p $n
p "v=#{e["SPX_W"]}"
r = []
e.each { |k, v| r << v if k == "SPX_W" }
p r
p e.to_h["SPX_W"], e.equal?(ENV), e.class, e.size == ENV.size
p e.map { |k, _| k }.include?("SPX_W")
p [e["SPX_W"], e.key?("nope")]
e.delete("SPX_W")
p ENV.key?("SPX_W"), $n
