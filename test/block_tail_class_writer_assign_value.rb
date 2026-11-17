# `Klass.x = v` as the last expression of a block a method yields to is an
# assignment: the block answers v, whatever the class's writer
# `def self.x=(v)` returns. The spliced block ran the writer as a statement,
# so the yield answered the writer's body, a String read as an Integer:
#
#   error: passing argument 1 of 'sp_box_int' makes integer from pointer
#   without a cast
class Config
  def self.limit=(n)
    @limit = n * 2
    "ignored"
  end

  def self.limit
    @limit
  end

  def self.reset
    wrap { self.limit = 0 }
  end
end

class Plain
end

def pick(n) = n.zero? ? Config : Plain

def try
  yield
rescue NoMethodError => e
  "#{e.class}: #{e.message}".split(" for ").first
end

def wrap
  yield
end

def keep
  r = yield
  r
end

def run(&blk) = blk.call

p try { Config.limit = 5 }
p Config.limit
k = pick(0)
p try { k.limit = 6 }
p Config.limit
k = pick(1)
p try { k.limit = 7 }
p keep { Config.limit = 8 }
p run { Config.limit = 9 }
p wrap { Config.limit += 1 }
p(wrap { p Config.limit; Config.limit = 3 } + 1)
p Config.reset
p Config.limit
