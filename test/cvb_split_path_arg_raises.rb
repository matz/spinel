# A computed send keeps the hook's parameter every class, so the boxed dispatch splits
# `proxy.new(store)` into a File.new arm; run, that arm raises TypeError, so it is
# not refused at compile time.
module App
  class Store
  end

  def self.store
    @store ||= Store.new
  end
end

class BaseProxy
  def self.proxies
    @@proxies ||= []
  end

  def self.inherited(klass)
    proxies << klass
  end

  def self.lookup(store)
    proxies.find { |proxy| proxy.handle?(store) }
  end
end

class Cache
  def self.default_store
    App.store
  end

  def initialize(store: self.class.default_store)
    self.store = store
  end

  def store=(store)
    @store = if proxy = BaseProxy.lookup(store)
      proxy.new(store)
    end
  end

  def store
    @store
  end
end

seen = [File, Dir, 1, "s", App::Store.new]
p seen.size
p Cache.new.store
p BaseProxy.proxies
p 7.send(ENV.fetch("SP_PROBE_NAME", "to_s"))
