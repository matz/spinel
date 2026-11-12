# Each Refused<i>.run refuses inside an inlined block body (the program
# never requires singleton). A refusal there used to leave the inline depth
# one higher, so after 64 of them Clean.run, which neither recurses nor
# refuses, was refused as a recursive yielding method.
module Partials
  def self.instance(i, n = nil)
    "p:#{i}"
  end
end

module Feeds
  class Registry
    include Singleton
  end
end

def wrap
  yield
end

class Refused0
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused1
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused2
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused3
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused4
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused5
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused6
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused7
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused8
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused9
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused10
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused11
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused12
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused13
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused14
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused15
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused16
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused17
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused18
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused19
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused20
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused21
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused22
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused23
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused24
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused25
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused26
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused27
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused28
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused29
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused30
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused31
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused32
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused33
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused34
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused35
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused36
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused37
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused38
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused39
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused40
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused41
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused42
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused43
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused44
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused45
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused46
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused47
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused48
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused49
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused50
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused51
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused52
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused53
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused54
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused55
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused56
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused57
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused58
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused59
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused60
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused61
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused62
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused63
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused64
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Refused65
  def self.run
    wrap { Feeds::Registry.instance }
  end
end

class Clean
  def self.run
    wrap { 42 }
  end
end

puts Clean.run
puts Partials.instance("x")
Refused0.run if ARGV.size > 99
Refused1.run if ARGV.size > 99
Refused2.run if ARGV.size > 99
Refused3.run if ARGV.size > 99
Refused4.run if ARGV.size > 99
Refused5.run if ARGV.size > 99
Refused6.run if ARGV.size > 99
Refused7.run if ARGV.size > 99
Refused8.run if ARGV.size > 99
Refused9.run if ARGV.size > 99
Refused10.run if ARGV.size > 99
Refused11.run if ARGV.size > 99
Refused12.run if ARGV.size > 99
Refused13.run if ARGV.size > 99
Refused14.run if ARGV.size > 99
Refused15.run if ARGV.size > 99
Refused16.run if ARGV.size > 99
Refused17.run if ARGV.size > 99
Refused18.run if ARGV.size > 99
Refused19.run if ARGV.size > 99
Refused20.run if ARGV.size > 99
Refused21.run if ARGV.size > 99
Refused22.run if ARGV.size > 99
Refused23.run if ARGV.size > 99
Refused24.run if ARGV.size > 99
Refused25.run if ARGV.size > 99
Refused26.run if ARGV.size > 99
Refused27.run if ARGV.size > 99
Refused28.run if ARGV.size > 99
Refused29.run if ARGV.size > 99
Refused30.run if ARGV.size > 99
Refused31.run if ARGV.size > 99
Refused32.run if ARGV.size > 99
Refused33.run if ARGV.size > 99
Refused34.run if ARGV.size > 99
Refused35.run if ARGV.size > 99
Refused36.run if ARGV.size > 99
Refused37.run if ARGV.size > 99
Refused38.run if ARGV.size > 99
Refused39.run if ARGV.size > 99
Refused40.run if ARGV.size > 99
Refused41.run if ARGV.size > 99
Refused42.run if ARGV.size > 99
Refused43.run if ARGV.size > 99
Refused44.run if ARGV.size > 99
Refused45.run if ARGV.size > 99
Refused46.run if ARGV.size > 99
Refused47.run if ARGV.size > 99
Refused48.run if ARGV.size > 99
Refused49.run if ARGV.size > 99
Refused50.run if ARGV.size > 99
Refused51.run if ARGV.size > 99
Refused52.run if ARGV.size > 99
Refused53.run if ARGV.size > 99
Refused54.run if ARGV.size > 99
Refused55.run if ARGV.size > 99
Refused56.run if ARGV.size > 99
Refused57.run if ARGV.size > 99
Refused58.run if ARGV.size > 99
Refused59.run if ARGV.size > 99
Refused60.run if ARGV.size > 99
Refused61.run if ARGV.size > 99
Refused62.run if ARGV.size > 99
Refused63.run if ARGV.size > 99
Refused64.run if ARGV.size > 99
Refused65.run if ARGV.size > 99
