# A `case` on an Integer that can be nil: integer labels stay a C switch
# (a nil subject matches none of them), and integer / range / nil labels test
# the typed value beside its nil flag instead of boxing the subject.

class Cpu
  def initialize(code)
    @code = code
    @op = nil
  end

  def fetch(i)
    @code[i]
  end

  def step(i)
    @op = fetch(i)
    case @op
    when 0 then "brk"
    when 1, 2 then "ora"
    when 0x69 then "adc"
    else "other"
    end
  end

  def step_stmt(i)
    @op = fetch(i)
    r = "?"
    case @op
    when 0
      r = "zero"
    when 3
      r = "three"
    else
      r = "else"
    end
    r
  end

  def no_else(i)
    @op = fetch(i)
    case @op
    when 0 then "z"
    when 1 then "o"
    end
  end

  def phase(i)
    @op = fetch(i)
    case @op
    when 2..8 then "mid"
    when 9.. then "high"
    when nil then "none"
    else "low"
    end
  end

  def phase_stmt(i)
    @op = fetch(i)
    out = "-"
    case @op
    when ..1
      out = "small"
    when 3, 10..12
      out = "pick"
    when nil
      out = "nil"
    end
    out
  end

  def mixed(i)
    @op = fetch(i)
    case @op
    when Integer then "int"
    when nil then "nil"
    end
  end
end

cpu = Cpu.new([0, 2, 0x69, 3, 7, 11, 1])
[0, 1, 2, 3, 4, 5, 6, 7, 100].each do |i|
  puts [cpu.step(i), cpu.step_stmt(i), cpu.no_else(i).inspect, cpu.phase(i),
        cpu.phase_stmt(i), cpu.mixed(i).inspect].join(" ")
end
