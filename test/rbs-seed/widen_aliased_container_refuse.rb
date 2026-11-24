# spinel: rbs-seed-check
# Retained or aliased containers whose variant differs from a wider RBS-seeded
# slot (Array[Integer] -> Array[untyped], Hash[String, String] ->
# Hash[String, untyped]) must be refused rather than silently copied, because a
# copy drops mutations visible through the original reference.

class WacrArr
  def f(a, b = a)
    b << 2
    b.equal?(a)
  end
end

class WacrCache
  def initialize
    @cache = { "a" => "old" }
  end

  def cached
    @cache
  end

  def mutate(h = cached)
    h["a"] = "changed"
    h
  end
end

class WacrBase
  def make_h
    { "a" => "1" }
  end

  def run(h = make_h)
    h["a"] = "2"
    h
  end
end

class WacrSub < WacrBase
  def initialize
    @cache = { "a" => "old" }
  end

  def make_h
    @cache
  end
end

class WacrIvar
  def set(x)
    @h = x
  end

  def h
    @h
  end
end

class WacrRec
  def rec_hash(n)
    n > 0 ? rec_hash(n - 1).freeze : { "a" => "1" }
  end

  def run_hash(h = rec_hash(1))
    h
  end

  def rec_arr(n)
    n > 0 ? rec_arr(n - 1).freeze : ["a"]
  end

  def run_arr(xs = rec_arr(1))
    xs
  end
end

def wacr_run_arr
  x = [1]
  WacrArr.new.f(x)
end

def wacr_run_cache
  WacrCache.new.mutate
end

def wacr_run_sub
  WacrSub.new.run
end

def wacr_run_rec_hash
  WacrRec.new.run_hash
end

def wacr_run_rec_arr
  WacrRec.new.run_arr
end

wacr_run_arr
wacr_run_cache
wacr_run_sub
wacr_run_rec_hash
wacr_run_rec_arr
b = WacrIvar.new
b.set({ "k" => "old" })
p b.h
