# A String Range with two all-digit ends holds the numbers between, each
# zero-padded to the begin's width (String#upto): include? answers from the
# asked string alone, as the walk did.
xs = ["0", "1", "01", "001", "9", "10", "11", "010", "100", "120", "121", "199999", "200000", "200001", "005", "5", "0005", "", "1a", "99", "12"]
rs = [
  ("1".."200000"), ("1"..."200000"),
  ("9".."11"), ("9"..."11"),
  ("1".."010"), ("1"..."010"),
  ("001".."120"), ("001"..."120"),
  ("0".."0"), ("0"..."0"),
  ("5".."3"), ("5"..."3"),
  ("10".."20"), ("10"..."20"),
  ("000".."005"), ("000"..."005"),
]
rs.each { |r| p [r.begin, r.end, r.exclude_end?, xs.map { |x| r.include?(x) ? 1 : 0 }.join] }
