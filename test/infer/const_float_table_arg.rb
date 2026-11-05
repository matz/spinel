# A constant table of Float rows passed to a class method, row by row. A
# local table of the same literal binds the parameter to a typed table of
# Float arrays; the constant must bind it the same way, or every helper that
# reads a row binds a boxed parameter and a boxed return.
class Curve
  XS = [0.0, 1.0, 2.0]
  TABLE = [[3.0, 2.0, 1.0], [22.0, 20.0, 10.0]].freeze

  def self.spline(xs, ys, v)
    return ys[0] if v <= xs[0]

    (1...xs.size).each do |i|
      return ys[i - 1] + (ys[i] - ys[i - 1]) * (v - xs[i - 1]) / (xs[i] - xs[i - 1]) if v < xs[i]
    end
    ys[xs.size - 1]
  end

  def self.land(table, row, v) = spline(XS, table[row], v)
end

p Curve.land(Curve::TABLE, 1, 0.5)
p Curve.land(Curve::TABLE, 0, 1.5).round
p Curve.land(Curve::TABLE, 1, 9.0)
