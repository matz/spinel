# A Hash subclass whose initialize yields is built inline at `new`; a keyword
# hash of two reads, taken whole by its positional parameter, is built from
# the reads once they have run.
class Store < Hash
  def initialize(source)
    source.each_pair { |k, v| self[k.to_s] = v }
    super()
  end
end

v = { name: "developer", uid: "dev@example.com" }
s = Store.new(provider: v[:name], uid: v[:uid])
p s["provider"]
p s["uid"]
p s.keys

log = []
t = Store.new(a: (log << 1; log.size), b: (log << 2; log.size))
p t
p log
