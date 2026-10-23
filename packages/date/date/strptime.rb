# frozen_string_literal: true

class Date
  class << self
    # call-seq:
    #   Date._strptime(string, format = '%F') -> hash
    #
    # Returns a hash of values parsed from +string+
    # according to the given +format+:
    #
    #   Date._strptime('2001-02-03', '%Y-%m-%d') # => {:year=>2001, :mon=>2, :mday=>3}
    #
    # For other formats, see
    # {Formats for Dates and Times}[rdoc-ref:language/strftime_formatting.rdoc].
    # (Unlike Date.strftime, does not support flags and width.)
    #
    # See also {strptime(3)}[https://man7.org/linux/man-pages/man3/strptime.3.html].
    #
    # Related: Date.strptime (returns a \Date object).
    def _strptime(string, format = '%F')
      string = string_value(string)
      format = string_value(format)
      if format == '%F' || format == '%Y-%m-%d'
        return internal_strptime_ymd(string)
      end
      if format == '%a %b %d %Y'
        return internal_strptime_abdy(string)
      end
      hash = {}
      si = catch(:sp_fail) { sp_run(string, 0, format, hash) }
      return nil unless si
      hash[:leftover] = string[si..] if si < string.length
      if (cent = hash.delete(:_cent))
        hash[:year]   = hash[:year]   + cent * 100 if hash.key?(:year)
        hash[:cwyear] = hash[:cwyear] + cent * 100 if hash.key?(:cwyear)
      end
      if (merid = hash.delete(:_merid))
        hash[:hour] = hash[:hour] % 12 + merid if hash.key?(:hour)
      end
      hash
    end

    # call-seq:
    #   Date.strptime(string = '-4712-01-01', format = '%F', start = Date::ITALY) -> date
    #
    # Returns a new \Date object with values parsed from +string+,
    # according to the given +format+:
    #
    #   Date.strptime('2001-02-03', '%Y-%m-%d')  # => #<Date: 2001-02-03>
    #   Date.strptime('03-02-2001', '%d-%m-%Y')  # => #<Date: 2001-02-03>
    #   Date.strptime('2001-034', '%Y-%j')       # => #<Date: 2001-02-03>
    #   Date.strptime('2001-W05-6', '%G-W%V-%u') # => #<Date: 2001-02-03>
    #   Date.strptime('2001 04 6', '%Y %U %w')   # => #<Date: 2001-02-03>
    #   Date.strptime('2001 05 6', '%Y %W %u')   # => #<Date: 2001-02-03>
    #   Date.strptime('sat3feb01', '%a%d%b%y')   # => #<Date: 2001-02-03>
    #
    # For other formats, see
    # {Formats for Dates and Times}[rdoc-ref:language/strftime_formatting.rdoc].
    # (Unlike Date.strftime, does not support flags and width.)
    #
    # See argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    #
    # See also {strptime(3)}[https://man7.org/linux/man-pages/man3/strptime.3.html].
    #
    # Related: Date._strptime (returns a hash).
    def strptime(string = JULIAN_EPOCH_DATE, format = '%F', start = DEFAULT_SG)
      str = string_value(string)
      if format == '%F' || format == '%Y-%m-%d'
        result = internalinternal_strptime_ymd_to_date(str, start)
        return result if result
        raise Error, 'invalid date'
      end
      if format == '%a %b %d %Y'
        result = internalinternal_strptime_abdy_to_date(str, start)
        return result if result
        raise Error, 'invalid date'
      end
      hash = _strptime(string, format)
      raise Error, 'invalid date' if hash.nil?
      if !hash.key?(:seconds) && !hash.key?(:leftover) &&
         !hash.key?(:_cent) && !hash.key?(:_merid) &&
         (year = hash[:year]) && (mon = hash[:mon]) && (mday = hash[:mday])
        jd = internal_valid_civil?(year, mon, mday, start)
        raise Error, 'invalid date' if jd.nil?
        return new_from_jd(jd, start)
      end
      internal_new_by_frags(hash, start)
    end


    private

    # Pre-compiled regex constants for sp_run
    SP_WHITESPACE     = /[ \t\n\r\v\f]+/
    SP_COLONS         = /:+/
    SP_EO_CHECK       = /[EO]/
    SP_E_COMBO        = /E[cCxXyY]/
    SP_O_COMBO        = /O[deHImMSuUVwWy]/
    SP_ALPHA3         = /[A-Za-z]{3}/
    SP_SIGN           = /[+-]/
    SP_SPACE_OR_DIGIT = / \d|\d{1,2}/
    SP_AMPM_DOT       = /[AaPp]\.[Mm]\./
    SP_AMPM           = /[AaPp][Mm]/
    SP_NUM_CHECK      = /\d|%[EO]?[CDdeFGgHIjkLlMmNQRrSsTUuVvWwXxYy0-9]/
    SP_DIGITS_1       = /\d/
    SP_DIGITS_2       = /\d{1,2}/
    SP_DIGITS_3       = /\d{1,3}/
    SP_DIGITS_4       = /\d{1,4}/
    SP_DIGITS_9       = /\d{1,9}/
    SP_DIGITS_MAX     = /\d+/
    private_constant :SP_WHITESPACE, :SP_COLONS, :SP_EO_CHECK, :SP_E_COMBO,
                     :SP_O_COMBO, :SP_ALPHA3, :SP_SIGN, :SP_SPACE_OR_DIGIT,
                     :SP_AMPM_DOT, :SP_AMPM, :SP_NUM_CHECK,
                     :SP_DIGITS_1, :SP_DIGITS_2, :SP_DIGITS_3, :SP_DIGITS_4,
                     :SP_DIGITS_9, :SP_DIGITS_MAX

    # Core scanner: walks format string and string simultaneously using StringScanner.
    # Returns new string position on success; throws :sp_fail on failure.
    def sp_run(str, si, fmt, hash)
      fmt_sc = StringScanner.new(fmt.dup)
      str_sc = StringScanner.new(str.dup)
      str_sc.pos = si

      until fmt_sc.eos?
        # Whitespace in format: skip any whitespace in both format and string
        if fmt_sc.skip(SP_WHITESPACE)
          str_sc.skip(SP_WHITESPACE)
          next
        end

        # Non-% literal: must match exactly
        unless fmt_sc.check(/%/)
          fc = fmt_sc.getch
          throw(:sp_fail) if str_sc.eos?
          throw(:sp_fail) if str_sc.getch != fc
          next
        end

        fmt_sc.skip(/%/) # skip '%'

        # Handle colon modifiers: %:z, %::z, %:::z
        if fmt_sc.scan(SP_COLONS)
          throw(:sp_fail) unless fmt_sc.skip(/z/)
          str_sc.pos = sp_zone(str, str_sc.pos, str.bytesize, hash)
          next
        end

        # Handle E/O locale modifiers
        if fmt_sc.check(SP_EO_CHECK)
          if fmt_sc.check(SP_E_COMBO) || fmt_sc.check(SP_O_COMBO)
            fmt_sc.skip(SP_EO_CHECK) # skip modifier, fall through to spec
          else
            # Invalid combo: match '%' literally in string
            throw(:sp_fail) if str_sc.eos? || str_sc.peek(1) != '%'
            str_sc.skip(/%/)
            next
          end
        end

        spec_ch = fmt_sc.getch
        spec = spec_ch&.ord

        case spec
        when 65, 97 # 'A', 'a'
          s3 = str_sc.scan(SP_ALPHA3)
          throw(:sp_fail) unless s3
          key = compute_3key(s3)
          entry = ABBR_DAY_3KEY[key]
          throw(:sp_fail) unless entry
          wday_i = entry[0]
          remaining = entry[1] - 3
          if remaining > 0
            tail = str_sc.peek(remaining)
            if tail.length == remaining && tail.downcase == DAY_LOWER_STRS[wday_i][3..]
              str_sc.pos += remaining
            end
          end
          hash[:wday] = wday_i

        when 66, 98, 104 # 'B', 'b', 'h'
          s3 = str_sc.scan(SP_ALPHA3)
          throw(:sp_fail) unless s3
          key = compute_3key(s3)
          entry = ABBR_MONTH_3KEY[key]
          throw(:sp_fail) unless entry
          mon_i = entry[0]
          remaining = entry[1] - 3
          if remaining > 0
            tail = str_sc.peek(remaining)
            if tail.length == remaining && tail.downcase == MONTH_LOWER_STRS[mon_i][3..]
              str_sc.pos += remaining
            end
          end
          hash[:mon] = mon_i

        when 67 # 'C'
          num_next = !fmt_sc.eos? && fmt_sc.check(SP_NUM_CHECK)
          if str_sc.scan(SP_SIGN)
            sign = str_sc.matched == '-' ? -1 : 1
          else
            sign = 1
          end
          s = str_sc.scan(num_next ? SP_DIGITS_2 : SP_DIGITS_MAX)
          throw(:sp_fail) unless s
          hash[:_cent] = sign * s.to_i

        when 99 # 'c'
          str_sc.pos = sp_run(str, str_sc.pos, '%a %b %e %H:%M:%S %Y', hash)

        when 68 # 'D'
          str_sc.pos = sp_run(str, str_sc.pos, '%m/%d/%y', hash)

        when 100, 101 # 'd', 'e'
          s = str_sc.scan(SP_SPACE_OR_DIGIT)
          throw(:sp_fail) unless s
          n = s.to_i
          throw(:sp_fail) if n < 1 || n > 31
          hash[:mday] = n

        when 70 # 'F'
          str_sc.pos = sp_run(str, str_sc.pos, '%Y-%m-%d', hash)

        when 71 # 'G'
          if str_sc.scan(SP_SIGN)
            sign = str_sc.matched == '-' ? -1 : 1
          else
            sign = 1
          end
          num_next = !fmt_sc.eos? && fmt_sc.check(SP_NUM_CHECK)
          s = str_sc.scan(num_next ? SP_DIGITS_4 : SP_DIGITS_MAX)
          throw(:sp_fail) unless s
          hash[:cwyear] = sign * s.to_i

        when 103 # 'g'
          s = str_sc.scan(SP_DIGITS_2)
          throw(:sp_fail) unless s
          n = s.to_i
          throw(:sp_fail) if n > 99
          hash[:cwyear] = n
          hash[:_cent] ||= n >= 69 ? 19 : 20

        when 72, 107 # 'H', 'k'
          s = str_sc.scan(SP_SPACE_OR_DIGIT)
          throw(:sp_fail) unless s
          n = s.to_i
          throw(:sp_fail) if n > 24
          hash[:hour] = n

        when 73, 108 # 'I', 'l'
          s = str_sc.scan(SP_SPACE_OR_DIGIT)
          throw(:sp_fail) unless s
          n = s.to_i
          throw(:sp_fail) if n < 1 || n > 12
          hash[:hour] = n

        when 106 # 'j'
          s = str_sc.scan(SP_DIGITS_3)
          throw(:sp_fail) unless s
          n = s.to_i
          throw(:sp_fail) if n < 1 || n > 366
          hash[:yday] = n

        when 76 # 'L'
          if str_sc.scan(SP_SIGN)
            sign = str_sc.matched == '-' ? -1 : 1
          else
            sign = 1
          end
          osi = str_sc.pos
          num_next = !fmt_sc.eos? && fmt_sc.check(SP_NUM_CHECK)
          s = str_sc.scan(num_next ? SP_DIGITS_3 : SP_DIGITS_MAX)
          throw(:sp_fail) unless s
          n = s.to_i
          n = -n if sign == -1
          hash[:sec_fraction] = Rational(n, 10**(str_sc.pos - osi))

        when 77 # 'M'
          s = str_sc.scan(SP_DIGITS_2)
          throw(:sp_fail) unless s
          n = s.to_i
          throw(:sp_fail) if n > 59
          hash[:min] = n

        when 109 # 'm'
          s = str_sc.scan(SP_DIGITS_2)
          throw(:sp_fail) unless s
          n = s.to_i
          throw(:sp_fail) if n < 1 || n > 12
          hash[:mon] = n

        when 78 # 'N'
          if str_sc.scan(SP_SIGN)
            sign = str_sc.matched == '-' ? -1 : 1
          else
            sign = 1
          end
          osi = str_sc.pos
          num_next = !fmt_sc.eos? && fmt_sc.check(SP_NUM_CHECK)
          s = str_sc.scan(num_next ? SP_DIGITS_9 : SP_DIGITS_MAX)
          throw(:sp_fail) unless s
          n = s.to_i
          n = -n if sign == -1
          hash[:sec_fraction] = Rational(n, 10**(str_sc.pos - osi))

        when 110, 116 # 'n', 't'
          str_sc.pos = sp_run(str, str_sc.pos, ' ', hash)

        when 80, 112 # 'P', 'p'
          throw(:sp_fail) if str_sc.eos?
          c0 = str_sc.peek(1)
          if c0 == 'P' || c0 == 'p'
            merid = 12
          elsif c0 == 'A' || c0 == 'a'
            merid = 0
          else
            throw(:sp_fail)
          end
          unless str_sc.scan(SP_AMPM_DOT) || str_sc.scan(SP_AMPM)
            throw(:sp_fail)
          end
          hash[:_merid] = merid

        when 81 # 'Q'
          sign = 1
          if str_sc.skip(/-/)
            sign = -1
          end
          s = str_sc.scan(SP_DIGITS_MAX)
          throw(:sp_fail) unless s
          n = s.to_i
          n = -n if sign == -1
          hash[:seconds] = Rational(n, 1000)

        when 82 # 'R'
          str_sc.pos = sp_run(str, str_sc.pos, '%H:%M', hash)

        when 114 # 'r'
          str_sc.pos = sp_run(str, str_sc.pos, '%I:%M:%S %p', hash)

        when 83 # 'S'
          s = str_sc.scan(SP_DIGITS_2)
          throw(:sp_fail) unless s
          n = s.to_i
          throw(:sp_fail) if n > 60
          hash[:sec] = n

        when 115 # 's'
          sign = 1
          if str_sc.skip(/-/)
            sign = -1
          end
          s = str_sc.scan(SP_DIGITS_MAX)
          throw(:sp_fail) unless s
          n = s.to_i
          n = -n if sign == -1
          hash[:seconds] = n

        when 84 # 'T'
          str_sc.pos = sp_run(str, str_sc.pos, '%H:%M:%S', hash)

        when 85 # 'U'
          s = str_sc.scan(SP_DIGITS_2)
          throw(:sp_fail) unless s
          n = s.to_i
          throw(:sp_fail) if n > 53
          hash[:wnum0] = n

        when 117 # 'u'
          s = str_sc.scan(SP_DIGITS_1)
          throw(:sp_fail) unless s
          n = s.to_i
          throw(:sp_fail) if n < 1 || n > 7
          hash[:cwday] = n

        when 86 # 'V'
          s = str_sc.scan(SP_DIGITS_2)
          throw(:sp_fail) unless s
          n = s.to_i
          throw(:sp_fail) if n < 1 || n > 53
          hash[:cweek] = n

        when 118 # 'v'
          str_sc.pos = sp_run(str, str_sc.pos, '%e-%b-%Y', hash)

        when 87 # 'W'
          s = str_sc.scan(SP_DIGITS_2)
          throw(:sp_fail) unless s
          n = s.to_i
          throw(:sp_fail) if n > 53
          hash[:wnum1] = n

        when 119 # 'w'
          s = str_sc.scan(SP_DIGITS_1)
          throw(:sp_fail) unless s
          n = s.to_i
          throw(:sp_fail) if n > 6
          hash[:wday] = n

        when 88 # 'X'
          str_sc.pos = sp_run(str, str_sc.pos, '%H:%M:%S', hash)

        when 120 # 'x'
          str_sc.pos = sp_run(str, str_sc.pos, '%m/%d/%y', hash)

        when 89 # 'Y'
          if str_sc.scan(SP_SIGN)
            sign = str_sc.matched == '-' ? -1 : 1
          else
            sign = 1
          end
          num_next = !fmt_sc.eos? && fmt_sc.check(SP_NUM_CHECK)
          s = str_sc.scan(num_next ? SP_DIGITS_4 : SP_DIGITS_MAX)
          throw(:sp_fail) unless s
          hash[:year] = sign * s.to_i

        when 121 # 'y'
          s = str_sc.scan(SP_DIGITS_2)
          throw(:sp_fail) unless s
          n = s.to_i
          throw(:sp_fail) if n > 99
          hash[:year] = n
          hash[:_cent] ||= n >= 69 ? 19 : 20

        when 90, 122 # 'Z', 'z'
          str_sc.pos = sp_zone(str, str_sc.pos, str.bytesize, hash)

        when 37 # '%'
          throw(:sp_fail) if str_sc.eos? || str_sc.peek(1) != '%'
          str_sc.skip(/%/)

        when 43 # '+'
          str_sc.pos = sp_run(str, str_sc.pos, '%a %b %e %H:%M:%S %Z %Y', hash)

        else
          # Unknown spec: match '%' then spec literally
          throw(:sp_fail) if str_sc.eos? || str_sc.peek(1) != '%'
          str_sc.skip(/%/)
          if spec_ch
            throw(:sp_fail) if str_sc.eos? || str_sc.peek(1) != spec_ch
            str_sc.getch
          end
        end
      end

      str_sc.pos
    end

    # Fast path for %Y-%m-%d / %F format.
    # Uses match? + byteslice to avoid StringScanner allocation overhead.
    STRPTIME_YMD_EXACT = /\A\d{4}-\d{2}-\d{2}\z/
    STRPTIME_YMD_PREFIX = /\A\d{4}-\d{2}-\d{2}/
    STRPTIME_YMD_GENERAL = /\A([+-]?\d+)-(\d{1,2})-(\d{1,2})(.*)\z/m
    private_constant :STRPTIME_YMD_EXACT, :STRPTIME_YMD_PREFIX, :STRPTIME_YMD_GENERAL

    def internal_strptime_ymd(str)
      slen = str.bytesize

      # Fast path for "YYYY-MM-DD" (exactly 10 chars)
      if slen == 10 && STRPTIME_YMD_EXACT.match?(str)
        year = str.byteslice(0, 4).to_i
        mon  = str.byteslice(5, 2).to_i
        mday = str.byteslice(8, 2).to_i
        return nil if mon < 1 || mon > 12 || mday < 1 || mday > 31
        return { year: year, mon: mon, mday: mday }
      end

      # Medium path for "YYYY-MM-DD..." (10+ chars, standard 4-digit year with leftover)
      if slen > 10 && STRPTIME_YMD_PREFIX.match?(str)
        year = str.byteslice(0, 4).to_i
        mon  = str.byteslice(5, 2).to_i
        mday = str.byteslice(8, 2).to_i
        if mon >= 1 && mon <= 12 && mday >= 1 && mday <= 31
          hash = { year: year, mon: mon, mday: mday }
          hash[:leftover] = str.byteslice(10..)
          return hash
        end
      end

      # General path for signed years, short years, etc.
      m = STRPTIME_YMD_GENERAL.match(str)
      return nil unless m
      year = m[1].to_i
      mon  = m[2].to_i
      mday = m[3].to_i
      return nil if mon < 1 || mon > 12 || mday < 1 || mday > 31
      hash = { year: year, mon: mon, mday: mday }
      rest = m[4]
      hash[:leftover] = rest unless rest.empty?
      hash
    end

    # Parse %Y-%m-%d and directly create Date object.
    # Returns Date object on success, nil on failure.
    # Uses match? + byteslice to avoid StringScanner allocation overhead.
    STRPTIME_YMD_GENERAL_EXACT = /\A([+-]?\d+)-(\d{1,2})-(\d{1,2})\z/
    private_constant :STRPTIME_YMD_GENERAL_EXACT

    def internalinternal_strptime_ymd_to_date(str, sg)
      slen = str.bytesize

      # Fast path for exactly "YYYY-MM-DD" (10 chars, positive 4-digit year)
      if slen == 10 && STRPTIME_YMD_EXACT.match?(str)
        year = str.byteslice(0, 4).to_i
        mon  = str.byteslice(5, 2).to_i
        mday = str.byteslice(8, 2).to_i
        if mon >= 1 && mon <= 12 && mday >= 1 && mday <= 31
          jd = internal_valid_civil?(year, mon, mday, sg)
          return jd ? new_from_jd(jd, sg) : nil
        end
        return nil
      end

      # General path for signed years, non-standard lengths
      m = STRPTIME_YMD_GENERAL_EXACT.match(str)
      return nil unless m
      year = m[1].to_i
      mon  = m[2].to_i
      mday = m[3].to_i
      return nil if mon < 1 || mon > 12 || mday < 1 || mday > 31

      jd = internal_valid_civil?(year, mon, mday, sg)
      return nil unless jd
      new_from_jd(jd, sg)
    end

    # Fast path for "%a %b %d %Y" format.
    # Uses single regex match to avoid StringScanner allocation.
    STRPTIME_ABDY_PAT = /\A([A-Za-z]{3})([A-Za-z]*) +([A-Za-z]{3})([A-Za-z]*) +(\d{1,2}) +([+-]?\d+)/
    private_constant :STRPTIME_ABDY_PAT

    def internal_strptime_abdy(str)
      m = STRPTIME_ABDY_PAT.match(str)
      return nil unless m

      # Validate weekday via 3-byte key lookup
      key = compute_3key(m[1])
      entry = ABBR_DAY_3KEY[key]
      return nil unless entry
      wday = entry[0]
      day_rest = m[2]
      unless day_rest.empty?
        return nil unless day_rest.length == entry[1] - 3 && day_rest.downcase == DAY_LOWER_STRS[wday][3..]
      end

      # Validate month via 3-byte key lookup
      key = compute_3key(m[3])
      entry = ABBR_MONTH_3KEY[key]
      return nil unless entry
      mon = entry[0]
      mon_rest = m[4]
      unless mon_rest.empty?
        return nil unless mon_rest.length == entry[1] - 3 && mon_rest.downcase == MONTH_LOWER_STRS[mon][3..]
      end

      mday = m[5].to_i
      return nil if mday < 1 || mday > 31
      year = m[6].to_i

      hash = { year: year, mon: mon, mday: mday, wday: wday }
      post = m.post_match
      hash[:leftover] = post unless post.empty?
      hash
    end

    # Parse "%a %b %d %Y" and directly create Date object.
    # Uses single regex match to avoid StringScanner allocation.
    def internalinternal_strptime_abdy_to_date(str, sg)
      m = STRPTIME_ABDY_PAT.match(str)
      return nil unless m
      return nil unless m.post_match.empty?

      # Validate weekday via 3-byte key lookup
      key = compute_3key(m[1])
      entry = ABBR_DAY_3KEY[key]
      return nil unless entry
      day_rest = m[2]
      unless day_rest.empty?
        return nil unless day_rest.length == entry[1] - 3 && day_rest.downcase == DAY_LOWER_STRS[entry[0]][3..]
      end

      # Validate month via 3-byte key lookup
      key = compute_3key(m[3])
      entry = ABBR_MONTH_3KEY[key]
      return nil unless entry
      mon = entry[0]
      mon_rest = m[4]
      unless mon_rest.empty?
        return nil unless mon_rest.length == entry[1] - 3 && mon_rest.downcase == MONTH_LOWER_STRS[mon][3..]
      end

      mday = m[5].to_i
      return nil if mday < 1 || mday > 31
      year = m[6].to_i

      jd = internal_valid_civil?(year, mon, mday, sg)
      return nil unless jd
      new_from_jd(jd, sg)
    end

    # Parse zone from string at position si; update hash[:zone] and hash[:offset].
    # Returns new si on success; throws :sp_fail on failure.
    def sp_zone(str, si, slen, hash)
      m = STRPTIME_ZONE_PAT.match(str[si..])
      throw(:sp_fail) unless m
      zone_str = m[1]
      hash[:zone]   = zone_str
      hash[:offset] = sp_zone_to_diff(zone_str)
      si + m[0].length
    end

    # Convert a zone string to seconds offset from UTC.
    # Returns Integer (seconds) or Rational, or nil if unparseable.
    # Ported from the original C date_zone_to_diff().
    def sp_zone_to_diff(zone_str)
      # Fast path for common numeric zones
      len = zone_str.length
      c0 = zone_str[0]
      if c0 == '+' || c0 == '-'
        sc = StringScanner.new(zone_str.dup)
        if sc.scan(/([+-])(\d{2}):?(\d{2})\z/)
          sign = sc[1] == '-' ? -1 : 1
          h = sc[2].to_i
          m = sc[3].to_i
          return nil if h > 23 || m > 59
          return sign * (h * 3600 + m * 60)
        end
      elsif len == 1 && (c0 == 'Z' || c0 == 'z')
        return 0
      elsif len <= 3
        off = ZONE_TABLE[zone_str.downcase]
        return off if off
      end

      s   = zone_str.dup
      dst = false

      # Strip trailing " time" (optionally preceded by "standard" or "daylight")
      strip_word = lambda do |str, len, word|
        n = word.length
        return nil unless len > n
        return nil unless str[len - n - 1] =~ /[[:space:]]/
        return nil unless str[len - n, n].casecmp(word) == 0
        n += 1
        n += 1 while len > n && str[len - n - 1] =~ /[[:space:]]/
        n
      end

      l = s.length
      if (w = strip_word.call(s, l, 'time'))
        l -= w
        if (w2 = strip_word.call(s, l, 'standard'))
          l -= w2
        elsif (w2 = strip_word.call(s, l, 'daylight'))
          l -= w2
          dst = true
        else
          l += w # revert
        end
      elsif (w = strip_word.call(s, l, 'dst'))
        l -= w
        dst = true
      end

      shrunk = s[0, l].gsub(/[[:space:]]+/, ' ').strip
      if (offset = ZONE_TABLE[shrunk.downcase])
        return dst ? offset + 3600 : offset
      end

      # Numeric parsing
      t = s[0, l].strip
      t = t[3..] if t =~ /\Agmt/i
      t = t[$&.length..] if t =~ /\Autc?/i
      return nil unless t && t.length > 0 && (t[0] == '+' || t[0] == '-')

      sign = t[0] == '-' ? -1 : 1
      t    = t[1..]

      if (m = t.match(/\A(\d+):(\d+)(?::(\d+))?\z/))
        h  = m[1].to_i
        mn = m[2].to_i
        sc = m[3] ? m[3].to_i : 0
        return nil if h > 23 || mn > 59 || sc > 59
        sign * (h * 3600 + mn * 60 + sc)
      elsif (m = t.match(/\A(\d+)[,.](\d*)/))
        h = m[1].to_i
        return nil if h > 23
        frac_s = m[2]
        n      = [frac_s.length, 7].min
        digits = frac_s[0, n].to_i
        digits += 1 if frac_s.length > n && frac_s[n].to_i >= 5
        sec = digits * 36
        os  = if n == 0
                h * 3600
              elsif n == 1
                sec * 10 + h * 3600
              elsif n == 2
                sec + h * 3600
              else
                denom = 10**(n - 2)
                r     = Rational(sec, denom) + h * 3600
                r.denominator == 1 ? r.numerator : r
              end
        sign == -1 ? -os : os
      elsif t =~ /\A(\d+)\z/
        digits = $1
        dlen   = digits.length
        h      = digits[0, 2 - dlen % 2].to_i
        mn     = dlen >= 3 ? digits[2 - dlen % 2, 2].to_i : 0
        sc     = dlen >= 5 ? digits[4 - dlen % 2, 2].to_i : 0
        sign * (h * 3600 + mn * 60 + sc)
      else
        nil
      end
    end

    # Rewrite :seconds (from %s/%Q) into jd + time components.
    # Offset is applied first (converts UTC epoch to local time).
    def sp_rewrite_frags(hash)
      seconds = hash.delete(:seconds)
      return hash unless seconds

      offset  = hash[:offset] || 0
      seconds = seconds + offset if offset != 0

      d,  fr = seconds.divmod(86400)
      h,  fr = fr.divmod(3600)
      m,  fr = fr.divmod(60)
      s,  fr = fr.divmod(1)

      hash[:jd]           = 2440588 + d
      hash[:hour]         = h
      hash[:min]          = m
      hash[:sec]          = s
      hash[:sec_fraction] = fr
      hash
    end

    # Complete partial date fragments by filling defaults from today's date.
    # Mirrors rt_complete_frags() in C.
    def sp_complete_frags(klass, hash)
      # Fast path: detect :civil case (most common) without iterating all entries
      k = nil
      a = nil
      if hash.key?(:year) || hash.key?(:mon) || hash.key?(:mday)
        civil_n = 0
        civil_n += 1 if hash.key?(:year)
        civil_n += 1 if hash.key?(:mon)
        civil_n += 1 if hash.key?(:mday)
        civil_n += 1 if hash.key?(:hour)
        civil_n += 1 if hash.key?(:min)
        civil_n += 1 if hash.key?(:sec)
        # Check if any other pattern matches better
        best_n = civil_n
        skip_civil = false
        if hash.key?(:jd) # jd entry has 1 element
          skip_civil = true if 1 > best_n
        end
        if hash.key?(:yday) # ordinal [:year, :yday, :hour, :min, :sec]
          ord_n = (hash.key?(:year) ? 1 : 0) + 1 + (hash.key?(:hour) ? 1 : 0) + (hash.key?(:min) ? 1 : 0) + (hash.key?(:sec) ? 1 : 0)
          skip_civil = true if ord_n > best_n
        end
        if hash.key?(:cwyear) || hash.key?(:cweek) || hash.key?(:cwday)
          com_n = (hash.key?(:cwyear) ? 1 : 0) + (hash.key?(:cweek) ? 1 : 0) + (hash.key?(:cwday) ? 1 : 0) + (hash.key?(:hour) ? 1 : 0) + (hash.key?(:min) ? 1 : 0) + (hash.key?(:sec) ? 1 : 0)
          skip_civil = true if com_n > best_n
        end
        if hash.key?(:wnum0)
          wn0_n = (hash.key?(:year) ? 1 : 0) + 1 + (hash.key?(:wday) ? 1 : 0) + (hash.key?(:hour) ? 1 : 0) + (hash.key?(:min) ? 1 : 0) + (hash.key?(:sec) ? 1 : 0)
          skip_civil = true if wn0_n > best_n
        end
        if hash.key?(:wnum1)
          wn1_n = (hash.key?(:year) ? 1 : 0) + 1 + (hash.key?(:wday) ? 1 : 0) + (hash.key?(:hour) ? 1 : 0) + (hash.key?(:min) ? 1 : 0) + (hash.key?(:sec) ? 1 : 0)
          skip_civil = true if wn1_n > best_n
        end
        unless skip_civil
          k = :civil
          a = [:year, :mon, :mday, :hour, :min, :sec]
        end
      end

      unless k
        best_k = nil
        best_a = nil
        best_n = 0
        COMPLETE_FRAGS_TAB.each do |ek, ea|
          n = ea.count { |sym| hash.key?(sym) }
          if n > best_n
            best_k = ek
            best_a = ea
            best_n = n
          end
        end
        k = best_k
        a = best_a
      end

      if k && best_n < a.length
        today = nil
        case k
        when :ordinal
          hash[:year] ||= (today ||= Date.today).year
          hash[:yday] ||= 1
        when :civil
          # (each field by name, as today.__send__(sym) answers it)
          a.each do |sym|
            break unless hash[sym].nil?
            today ||= Date.today
            hash[sym] = case sym
                        when :year then today.year
                        when :mon then today.mon
                        when :mday then today.mday
                        else raise NoMethodError, "undefined method '#{sym}' for an instance of Date"
                        end
          end
          hash[:mon]  ||= 1
          hash[:mday] ||= 1
        when :commercial
          a.each do |sym|
            break unless hash[sym].nil?
            today ||= Date.today
            hash[sym] = case sym
                        when :cwyear then today.cwyear
                        when :cweek then today.cweek
                        when :cwday then today.cwday
                        else raise NoMethodError, "undefined method '#{sym}' for an instance of Date"
                        end
          end
          hash[:cweek] ||= 1
          hash[:cwday] ||= 1
        when :wday
          today ||= Date.today
          hash[:jd] = (today - today.wday + hash[:wday]).jd
        when :wnum0
          a.each do |sym|
            break unless hash[sym].nil?
            hash[sym] = (today ||= Date.today).year
          end
          hash[:wnum0] ||= 0
          hash[:wday]  ||= 0
        when :wnum1
          a.each do |sym|
            break unless hash[sym].nil?
            hash[sym] = (today ||= Date.today).year
          end
          hash[:wnum1] ||= 0
          hash[:wday]  ||= 1
        end
      end

      if k == :time && klass <= DateTime
        hash[:jd] ||= Date.today.jd
      end

      hash[:hour] ||= 0
      hash[:min]  ||= 0
      hash[:sec]  = if hash[:sec].nil?
                      0
                    elsif hash[:sec] > 59
                      59
                    else
                      hash[:sec]
                    end
      hash
    end

    # Convert year/week/wday to Julian Day number.
    # f=0: Sunday-based (%U), d=0=Sun..6=Sat
    # f=1: Monday-based (%W), d=0=Mon..6=Sun (Mon-based)
    # Ported from the original C c_weeknum_to_jd():
    #   rjd2 = JD(Jan 1) + 6  (= JD of Jan 7)
    #   return (rjd2 - MOD((rjd2 - f + 1), 7) - 7) + 7*w + d
    def sp_weeknum_to_jd(y, w, d, f, sg)
      jd_jan7 = civil_to_jd(y, 1, 1, sg) + 6
      (jd_jan7 - (jd_jan7 - f + 1) % 7 - 7) + 7 * w + d
    end

    # Find the Julian Day number from the fragment hash.
    # Tries jd, ordinal, civil, commercial, wnum0, wnum1 in order.
    # Returns jd integer or nil.
    def sp_valid_date_frags_p(hash, sg)
      return hash[:jd] if hash[:jd]

      if (yday = hash[:yday]) && (year = hash[:year])
        jd = internal_valid_ordinal?(year, yday, sg)
        return jd if jd
      end

      if (mday = hash[:mday]) && (mon = hash[:mon]) && (year = hash[:year])
        jd = internal_valid_civil?(year, mon, mday, sg)
        return jd if jd
      end

      # Commercial (ISO week): prefer cwday, else wday (treating 0 as 7)
      wday = hash[:cwday]
      if wday.nil?
        wday = hash[:wday]
        wday = 7 if !wday.nil? && wday == 0
      end
      if wday && (week = hash[:cweek]) && (year = hash[:cwyear])
        jd = internal_valid_commercial?(year, week, wday, sg)
        return jd if jd
      end

      # wnum0 (Sunday-start): prefer wday (0=Sun), else cwday (converting 7→0)
      wday = hash[:wday]
      if wday.nil?
        wday = hash[:cwday]
        wday = 0 if !wday.nil? && wday == 7
      end
      if wday && (week = hash[:wnum0]) && (year = hash[:year])
        jd = sp_weeknum_to_jd(year, week, wday, 0, sg)
        return jd if jd
      end

      # wnum1 (Monday-start): convert to Mon-based 0=Mon using (wday-1)%7
      # Uses original wday (0=Sun) or cwday (1=Mon..7=Sun) — no 7→0 conversion here
      wday = hash[:wday]
      wday = hash[:cwday] if wday.nil?
      wday = (wday - 1) % 7 if wday
      if wday && (week = hash[:wnum1]) && (year = hash[:year])
        jd = sp_weeknum_to_jd(year, week, wday, 1, sg)
        return jd if jd
      end

      nil
    end

    # Create a Date object from parsed fragment hash.
    def internal_new_by_frags(hash, sg)
      raise Error, 'invalid date' if hash.nil?
      hash = sp_rewrite_frags(hash)
      hash = sp_complete_frags(Date, hash)
      jd   = sp_valid_date_frags_p(hash, sg)
      raise Error, 'invalid date' if jd.nil?
      new_from_jd(jd, sg)
    end
  end
end
