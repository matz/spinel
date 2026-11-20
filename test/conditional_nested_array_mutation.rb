def append_row(flag)
  left = [[1]]
  right = [[2]]
  rows = flag ? left : right
  rows.each { |row| row.push(:changed) }
  p left, right
end

append_row(true)
append_row(false)

# Every alternative can supply a row, including an earlier assignment.
def visit(n)
  rows = [[-1]]
  if n >= 0
    rows = if n < 33
      [
        [[0]], [[1]], [[2]], [[3]], [[4]], [[5]], [[6]], [[7]],
        [[8]], [[9]], [[10]], [[11]], [[12]], [[13]], [[14]], [[15]],
        [[16]], [[17]], [[18]], [[19]], [[20]], [[21]], [[22]], [[23]],
        [[24]], [[25]], [[26]], [[27]], [[28]], [[29]], [[30]], [[31]], [[32]]
      ].fetch(n)
    end
  end
  rows.each { |row| row << :visited }
  p rows
end

visit(-1)
visit(32)
