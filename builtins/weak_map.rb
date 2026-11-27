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

    def inspect
      "#<ObjectSpace::WeakMap #{@map.map { |k, v| "#{k.inspect} => #{v.inspect}" }.join(", ")}>"
    end
  end
end
