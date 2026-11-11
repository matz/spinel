# A method that yields is emitted inline at its call, and its value there is
# that call's: a row literal it ends with is built as the row the call
# answers, also when the caller's own value is a table of Integer rows (the
# caller's return type built it as a table, which did not compile).

class Rows
  def plain(s) = [s.size, 1]

  def yielding(s)
    yield
    [s.size, 2]
  end

  def returning(s)
    yield
    return [0, 0] if s.empty?
    [s.size, 3]
  end

  def run
    r = []
    r << plain("ab")
    r << yielding("abc") { 0 }
    r << returning("abcd") { 0 }
    r << returning("") { 0 }
    r
  end
end

class Base
  def row(s)
    yield
    [s.size, 4]
  end
end

class Kid < Base
  def row(s)
    super
  end

  def run
    t = []
    t << row("xyz") { 0 }
    t << [9, 9]
    t
  end
end

x = Rows.new.run
x.each { |y| p y }
k = Kid.new.run
k.each { |y| p y }
p x.sum { |y| y[0] }, k.length
