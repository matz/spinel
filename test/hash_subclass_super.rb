# A Hash subclass whose overrides call super, as rack's Rack::Headers does:
# keys go in and out lower-cased
class Headers < Hash
  def self.[](*pairs)
    h = new
    pairs.each_slice(2) { |k, v| h[k] = v }
    h
  end

  def [](key) = super(key.downcase)

  def []=(key, value)
    super(key.downcase, value)
  end

  def key?(key) = super(key.downcase)
  def delete(key) = super(key.downcase)

  def fetch(key, *default, &block)
    key = key.downcase
    super
  end

  def update(hash, &block)
    hash.each do |key, value|
      self[key] = if block_given? && include?(key.downcase)
        block.call(key, self[key], value)
      else
        value
      end
    end
    self
  end
  alias merge! update

  def merge(hash, &block) = dup.merge!(hash, &block)

  def invert
    h = self.class.new
    each { |k, v| h[v] = k }
    h
  end

  def values_at(*keys) = keys.map { |k| self[k] }
end

h = Headers.new
h["Content-Type"] = "text/html"
h["X-Custom"] = "1"
p h["CONTENT-TYPE"], h.key?("x-CUSTOM"), h.fetch("Content-type"), h.fetch("nope", "d"), h.fetch("Nope") { |k| k }
p h, h.size, h.class, h.is_a?(Hash)
h.delete("X-CUSTOM")
m = h.merge("Cache-Control" => "no-cache")
p m.class, m, h.size
h.update("Content-Type" => "a") { |k, o, n| "#{o}+#{n}" }
p h["content-type"]
p Headers["A", 1, "B", 2], h.invert.class, h.invert
p h.values_at("CONTENT-TYPE", "missing"), h.to_h.class
h.each { |k, v| puts "#{k}: #{v}" }
