# A String argument narrowed out of a boxed parameter (`if name.is_a?(String)`)
# handed to a boxed receiver: an ivar that starts as {} and is later given
# another Hash, a String or an Array. key?, include?, member?, has_key?,
# delete, fetch and dig take it; the generated C bound the parameter's box to a
# `const char *` and did not compile.
class NbEnv
  def initialize = @env = {}
  def set(env) = @env = env
  def get(name)
    if name.is_a?(String)
      return @env[name] if @env.key?(name)
    end
    name
  end
  def all(name)
    out = []
    if name.is_a?(String)
      out << @env.key?(name) << @env.include?(name) << @env.member?(name) << @env.has_key?(name)
      out << @env[name]
      out << (@env.delete(name) ? :deleted : :kept)
    end
    out
  end
  def fet(name)
    if name.is_a?(String)
      return [@env.fetch(name, :none), @env.dig(name), @env.fetch(name) { |k| k + "!" }]
    end
    name
  end
  def str(name)
    if name.is_a?(String)
      [@env.include?(name), @env.index(name), @env.start_with?(name), @env.count(name), @env + name, @env == name]
    end
  end
  def arr(name)
    if name.is_a?(String)
      [@env.include?(name), @env.index(name), @env.count(name), @env.push(name).size, @env.delete(name)]
    end
  end
end
c = NbEnv.new
c.set({ "T" => "i64" })
p c.get("T"), c.get(:u8)
p c.all("T"), c.all("U"), c.all(:u8)
p c.fet("T"), c.fet("U"), c.fet(:u8)
c.set({ "T" => 1 })
p c.fet("T"), c.fet("U")
c.set("abcT")
p c.str("T"), c.str(:x)
c.set(["T", "U"])
p c.arr("T"), c.arr(3)
c.set({ T: 1 })
p c.all("T")
