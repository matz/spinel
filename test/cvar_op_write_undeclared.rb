# `@@n += 1` and `@@n &&= v` read the class variable first. When no write
# declares it, CRuby raises NameError at that read, as for a plain read.
class Counter
  def self.bump = (@@n += 1)
  def self.read = @@n
  def self.and = (@@n &&= 2)
end
p((Counter.bump rescue $!.message))
p((Counter.read rescue $!.class))
p((Counter.and rescue $!.message))
class S
  def self.app = (@@s += "x")
end
p((S.app rescue $!.message))
class Body
  begin
    @@b -= 1
  rescue NameError => e
    p e.name
  end
end
# A variable another write declares keeps working.
class Base
  @@count = 0
  def self.bump = (@@count += 1)
  def self.count = @@count
end
class Kid < Base
  def self.kbump = (@@count += 10)
end
Base.bump
Kid.kbump
p Base.count
class Or
  def self.inc = (@@o ||= 0; @@o += 2)
end
p Or.inc
p Or.inc
module M
  @@m = [1]
  def self.add = (@@m += [2])
end
p M.add
class Set3; end
Set3.class_variable_set(:@@v, 3)
class Set3
  def self.inc = (@@v += 1)
end
p Set3.inc
