class Store
  class << self
    def items
      Thread.current[:items] ||= []
    end

    def flush!
      Thread.current[:items] = nil
    end
  end
end

Store.items << :a
Store.items << :b
p Store.items
Store.flush!
p Store.items
p(Thread.current[:count] ||= 1)
p(Thread.current[:count] ||= 2)
Thread.current[:count] += 10
p Thread.current[:count]
p(Thread.current[:count] &&= 5)
p(Thread.current[:missing] &&= 5)
