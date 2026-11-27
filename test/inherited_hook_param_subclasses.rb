# A class's `inherited` hears of its own subclasses only. Its parameter held
# every value the runtime had seen (File and Dir here), so `proxy.new(store)`
# took File.new's arm and refused the store as a path (rack-attack's store
# proxies, where no proxy takes the app's cache).
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
