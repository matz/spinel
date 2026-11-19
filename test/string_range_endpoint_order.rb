# frozen_string_literal: true
lo = +"abc"; hi = +"def"; saved = lo
r = lo..(lo = +"other"; hi)
p [r.begin, r.begin.equal?(saved)]
lo = +"abc"; saved = lo
r = Range.new(lo, (lo = +"other"; hi))
p [r.begin, r.begin.equal?(saved)]
