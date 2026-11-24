# A call that is an element of an Array, a Hash or a keyword list runs whenever its statement does, so a hook the
# method it calls defines (method_added from a class method's body) is in force at the statement after it; where
# the call may not run (a safe navigation's argument, a block, a branch) the hook is not known
# to be defined and the later def runs the hook only if the call ran.
def kw(**o) = o

class Ar
  def self.set
    def self.method_added(n) = puts("ar #{n}")
    1
  end
  p [0, set, 2]
  def z; end
end

class Ha
  def self.set
    def self.method_added(n) = puts("ha #{n}")
    1
  end
  p({ a: 0, b: set })
  def z; end
end

class Kw
  def self.set
    def self.method_added(n) = puts("kw #{n}")
    1
  end
  kw(a: set)
  def z; end
end

class Sp
  def self.set
    def self.method_added(n) = puts("sp #{n}")
    { a: 1 }
  end
  kw(**set)
  def z; end
end

class St
  def self.set
    def self.method_added(n) = puts("st #{n}")
    [1]
  end
  p [*set, 2]
  def z; end
end

class Rs
  def self.set
    def self.method_added(n) = puts("rs #{n}")
    1
  end
  def self.boom = raise("boom")
  begin
    p [set, boom]
  rescue => e
    puts e.message
  end
  def z; end
end

class Sn
  def self.set
    def self.method_added(n) = puts("sn #{n}")
    1
  end
  p [nil&.foo(set)]
  def z; end
end

class Bl
  def self.set
    def self.method_added(n) = puts("bl #{n}")
    1
  end
  p [[1].map { set }]
  def z; end
end

class Br
  def self.set
    def self.method_added(n) = puts("br #{n}")
    1
  end
  p [false ? set : 0]
  def z; end
end

class Sh
  def self.set
    def self.method_added(n) = puts("sh #{n}")
    1
  end
  p [false && set]
  def z; end
end
