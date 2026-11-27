# A split arm's TypeError comes after its later arguments run, as CRuby's does
class Store
end

class Opener
  def initialize(store, mode)
    @store = store
    @mode = mode
  end

  def mode
    @mode
  end
end

def mode_of(log)
  log << "mode"
  "r"
end

def open_with(klass, store, log)
  klass.new(store, mode_of(log))
end

log = []
kinds = [Opener, File]
p open_with(kinds[0], Store.new, log).mode
begin
  open_with(kinds[1], Store.new, log)
rescue TypeError => e
  p e.class
end
p log
p 7.send(ENV.fetch("SP_PROBE_NAME", "to_s"))
