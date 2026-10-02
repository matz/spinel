# A class whose superclass is Hash (`class Headers < Hash`) includes a copy
# of this module of its own, X__SpinelHashBase, which the parser defines on
# the class's line (a module of its own is typed for that class alone). The instance stays an
# object of its own class, with its own instance variables, and keeps the
# Hash it is in @__spinel_base; Hash's methods forward to it. An override's
# `super` reaches the method here, as it would reach Hash's.
#
# Methods that answer the receiver answer the object, not the Hash in it.
# The ones CRuby answers with the subclass (dup, clone, merge, compact) do
# too; the rest answer a plain Hash, as CRuby does.
module SpinelHashBase
  include Enumerable

  # Hash.new and Hash.new(default); a default block is not supported here
  def initialize(*args)
    raise NotImplementedError, "a Hash subclass with a default block" if block_given?
    # one Hash of any keys and values, whatever this class stores
    @__spinel_base = Hash.new(args[0])
  end

  # the copy starts with the original's ivars: its own Hash from here on
  def initialize_copy(orig)
    @__spinel_base = @__spinel_base.dup
  end

  # the Hash, for another object of such a class (other.__spinel_base);
  # the methods here read the ivar itself, which the allocation seeds
  def __spinel_base = @__spinel_base

  def [](key) = @__spinel_base[key]

  def []=(key, value)
    __spinel_modify
    @__spinel_base[key] = value
  end

  def store(key, value)
    __spinel_modify
    @__spinel_base[key] = value
  end

  def fetch(key, *default)
    if block_given?
      @__spinel_base.fetch(key) { |k| yield k }
    elsif default.empty?
      @__spinel_base.fetch(key)
    else
      @__spinel_base.fetch(key, default[0])
    end
  end

  def key?(key) = @__spinel_base.key?(key)
  def has_key?(key) = @__spinel_base.key?(key)
  def include?(key) = @__spinel_base.key?(key)
  def member?(key) = @__spinel_base.key?(key)
  def value?(value) = @__spinel_base.value?(value)
  def has_value?(value) = @__spinel_base.value?(value)
  def key(value) = @__spinel_base.key(value)

  def delete(key)
    __spinel_modify
    if block_given?
      @__spinel_base.delete(key) { |k| yield k }
    else
      @__spinel_base.delete(key)
    end
  end

  def dig(key, *rest)
    v = self[key]
    i = 0
    while i < rest.size && !v.nil?
      v = v.dig(rest[i])
      i += 1
    end
    v
  end

  def values_at(*keys) = keys.map { |k| self[k] }
  def fetch_values(*keys) = keys.map { |k| @__spinel_base.fetch(k) }
  def assoc(key) = @__spinel_base.assoc(key)
  def rassoc(value) = @__spinel_base.rassoc(value)

  def keys = @__spinel_base.keys
  def values = @__spinel_base.values
  def size = @__spinel_base.size
  def length = @__spinel_base.size
  def empty? = @__spinel_base.empty?
  def to_a = @__spinel_base.to_a
  def to_hash = @__spinel_base
  def to_proc = ->(k) { self[k] }
  def inspect = @__spinel_base.inspect
  def to_s = @__spinel_base.inspect
  def hash = @__spinel_base.hash
  def default = @__spinel_base.default
  def default_proc = nil
  def compare_by_identity? = @__spinel_base.compare_by_identity?
  def shift
    __spinel_modify
    @__spinel_base.shift
  end
  def invert = @__spinel_base.invert
  def flatten(depth = 1) = @__spinel_base.flatten(depth)
  def deconstruct_keys(keys) = @__spinel_base

  # frozen with its Hash, which then refuses every store
  def freeze
    @__spinel_base.freeze
    super
  end

  # Enumerable#sum, which a boxed object of the class reaches through its
  # own arm
  def sum(init = 0)
    return @__spinel_base.sum(init) unless block_given?
    @__spinel_base.sum(init) { |k, v| yield k, v }
  end

  def frozen? = @__spinel_base.frozen?

  # CRuby names the class the store was refused on
  def __spinel_modify
    raise FrozenError, "can't modify frozen #{self.class}: #{@__spinel_base.inspect}" if @__spinel_base.frozen?
  end

  def default=(value)
    __spinel_modify
    @__spinel_base.default = value
  end

  def to_h
    return @__spinel_base.dup unless block_given?
    @__spinel_base.to_h { |k, v| yield k, v }
  end

  def ==(other)
    return @__spinel_base == other.__spinel_base if other.respond_to?(:__spinel_base)
    @__spinel_base == other
  end

  def eql?(other)
    return @__spinel_base.eql?(other.__spinel_base) if other.respond_to?(:__spinel_base)
    @__spinel_base.eql?(other)
  end

  def each
    return @__spinel_base.each unless block_given?
    @__spinel_base.each { |k, v| yield [k, v] }
    self
  end

  def each_pair
    return @__spinel_base.each_pair unless block_given?
    @__spinel_base.each { |k, v| yield [k, v] }
    self
  end

  def each_with_index
    return @__spinel_base.each_with_index unless block_given?
    i = 0
    @__spinel_base.each do |k, v|
      yield [k, v], i
      i += 1
    end
    self
  end

  def each_key
    return @__spinel_base.each_key unless block_given?
    @__spinel_base.each_key { |k| yield k }
    self
  end

  def each_value
    return @__spinel_base.each_value unless block_given?
    @__spinel_base.each_value { |v| yield v }
    self
  end

  def any?
    return !@__spinel_base.empty? unless block_given?
    @__spinel_base.any? { |k, v| yield k, v }
  end

  def count
    return @__spinel_base.size unless block_given?
    @__spinel_base.count { |k, v| yield k, v }
  end

  def select
    return @__spinel_base.select unless block_given?
    @__spinel_base.select { |k, v| yield k, v }
  end
  def filter
    return @__spinel_base.select unless block_given?
    @__spinel_base.select { |k, v| yield k, v }
  end
  def reject
    return @__spinel_base.reject unless block_given?
    @__spinel_base.reject { |k, v| yield k, v }
  end
  def slice(*keys) = @__spinel_base.slice(*keys)
  def except(*keys) = @__spinel_base.except(*keys)
  def transform_values
    return @__spinel_base.transform_values unless block_given?
    @__spinel_base.transform_values { |v| yield v }
  end
  def transform_keys
    return @__spinel_base.transform_keys unless block_given?
    @__spinel_base.transform_keys { |k| yield k }
  end

  def select!
    __spinel_modify
    return @__spinel_base.select! unless block_given?
    r = @__spinel_base.select! { |k, v| yield k, v }
    r.nil? ? nil : self
  end

  def filter!
    __spinel_modify
    return @__spinel_base.select! unless block_given?
    r = @__spinel_base.select! { |k, v| yield k, v }
    r.nil? ? nil : self
  end

  def reject!
    __spinel_modify
    return @__spinel_base.reject! unless block_given?
    r = @__spinel_base.reject! { |k, v| yield k, v }
    r.nil? ? nil : self
  end

  def keep_if
    __spinel_modify
    return @__spinel_base.keep_if unless block_given?
    @__spinel_base.keep_if { |k, v| yield k, v }
    self
  end

  def delete_if
    __spinel_modify
    return @__spinel_base.delete_if unless block_given?
    @__spinel_base.delete_if { |k, v| yield k, v }
    self
  end

  def transform_values!
    __spinel_modify
    return @__spinel_base.transform_values! unless block_given?
    @__spinel_base.transform_values! { |v| yield v }
    self
  end

  def transform_keys!
    __spinel_modify
    return @__spinel_base.transform_keys! unless block_given?
    @__spinel_base.transform_keys! { |k| yield k }
    self
  end

  def compact
    h = dup
    h.compact!
    h
  end

  def compact!
    __spinel_modify
    r = @__spinel_base.compact!
    r.nil? ? nil : self
  end

  def update(other)
    __spinel_modify
    src = other.respond_to?(:__spinel_base) ? other.__spinel_base : other
    if block_given?
      src.each { |k, v| @__spinel_base[k] = @__spinel_base.key?(k) ? yield(k, @__spinel_base[k], v) : v }
    else
      @__spinel_base.update(src)
    end
    self
  end

  def merge!(other)
    __spinel_modify
    src = other.respond_to?(:__spinel_base) ? other.__spinel_base : other
    if block_given?
      src.each { |k, v| @__spinel_base[k] = @__spinel_base.key?(k) ? yield(k, @__spinel_base[k], v) : v }
    else
      @__spinel_base.update(src)
    end
    self
  end

  def merge(other)
    h = dup
    if block_given?
      h.update(other) { |k, a, b| yield k, a, b }
    else
      h.update(other)
    end
    h
  end

  def replace(other)
    __spinel_modify
    src = other.respond_to?(:__spinel_base) ? other.__spinel_base : other
    @__spinel_base.replace(src)
    self
  end

  def clear
    __spinel_modify
    @__spinel_base.clear
    self
  end

  def rehash
    __spinel_modify
    @__spinel_base.rehash
    self
  end

  def compare_by_identity
    __spinel_modify
    @__spinel_base.compare_by_identity
    self
  end
end
