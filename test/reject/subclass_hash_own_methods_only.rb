# A subclass of Hash that only calls its own methods still answers p, to_s,
# == and respond_to? as an Object would (#7075): refused all the same.
class Opts < Hash
  def describe = "opts"
end

o = Opts.new
p o.describe
p o
