# ENV held where nothing else is held (a local, a parameter, a parameter's
# default, an instance variable) is ENV itself, not a copy of the
# environment: a write through it changes the environment, a read sees the
# variables set later, and it is equal? to ENV.
ENV["SPX_A"] = "1"
x = ENV
x["SPX_B"] = "2"
p ENV["SPX_B"]
p x.equal?(ENV), x == ENV, x.nil?
def wr(e) = (e["SPX_C"] = "3")
wr(ENV)
p ENV["SPX_C"]
ENV["SPX_D"] = "4"
p x["SPX_D"]
p x.key?("SPX_A"), x.fetch("SPX_NO", "dflt")
p x.select { |k, _| k.start_with?("SPX_") }.keys.sort
p x.delete("SPX_D"), ENV.key?("SPX_D")

class Holder
  def initialize = @env = ENV
  def get(k) = @env[k]
  def put(k, v) = (@env[k] = v)
end
h = Holder.new
h.put("SPX_E", "5")
p ENV["SPX_E"], h.get("SPX_A")

def with_default(env = ENV) = env["SPX_A"]
p with_default
