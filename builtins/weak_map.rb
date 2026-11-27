# ObjectSpace::WeakMap, spliced by the parser when a program names it: a map
# keyed by object identity, as CRuby's is (`compare_by_identity`). CRuby's
# holds its keys and values weakly, and an entry goes when the collector
# recycles its key; Spinel's collector hands out no weak references, so the
# map keeps its entries alive, as the weakref library's WeakRef keeps its
# object. The methods are CRuby's: [] / []=, key? (include?, member?),
# delete, each (each_pair), each_key, each_value, keys, values, size
# (length), inspect.
module ObjectSpace
  class WeakMap
    include Enumerable

    def initialize
      @map = {}.compare_by_identity
    end

    def [](key) = @map[key]

    def []=(key, value)
      @map[key] = value
    end

    def key?(key) = @map.key?(key)
    alias include? key?
    alias member? key?

    def delete(key)
      if @map.key?(key)
        @map.delete(key)
      elsif block_given?
        yield key
      end
    end

    def each(&block)
      @map.each(&block)
      self
    end
    alias each_pair each

    def each_key(&block)
      @map.each_key(&block)
      self
    end

    def each_value(&block)
      @map.each_value(&block)
      self
    end

    def keys = @map.keys
    def values = @map.values
    def size = @map.size
    alias length size

    # CRuby's inspect writes an immediate entry as its inspect and any other
    # object in its default form (rb_any_to_s), never through the object's
    # own inspect: an entry holding the map itself (`m[k] = m`) is finite
    def inspect
      s = "#<ObjectSpace::WeakMap:0x#{__addr(self)}"
      s << ": " << @map.map { |k, v| "#{__entry(k)} => #{__entry(v)}" }.join(", ") unless @map.empty?
      s << ">"
    end

    def __addr(x) = x.object_id.to_s(16).rjust(16, "0")

    def __entry(x)
      case x
      when nil, true, false, Integer, Float, Symbol then x.inspect
      else "#<#{x.class}:0x#{__addr(x)}>"
      end
    end
  end
end
