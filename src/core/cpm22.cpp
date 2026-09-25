// CPCSyntaxError — CP/M 2.2 system image.
#include "cpm22.h"

namespace cpcse {

static const char* CPM22_BASE64 =
    "YGkRAAUBMwDtsA5CEQAAIQADzYm+MAbNUAHDAwXNWQINCkZhaWxlZCB0byBsb2FkIHRoZSBjb25maWd1cmF0aW9uIHNlY3Rvcg0K"
    "CiTDAwXNWQIEAiQqAAMRy+0ZfLUoJs1ZAg0KSWxsZWdhbCBjb25maWd1cmF0aW9uIHNlY3Rvcg0KCiTJKgIDIj0CKgQDIj8COgYD"
    "MkMCIT0CzYO+OgcDMgMAOggDzYC+OgkDzZ6+IQoDzaG+IWQDzV8CfiO3KAxHTiPlxc0PBcHhEPXrISe7zUYCIS27zUYCITO7zUYC"
    "634jtyhQV0YjTiPl1cXND7vB0eE4Os1ZAg0KRXhwYW5zaW9uIGJ1ZmZlciBmdWxsIG9yIGlsbGVnYWwgdG9rZW4gc3BlY2lmaWVk"
    "DQoKJMkGAAkVILE6FgPDpL4AAAAArx4AAQMaTxO3yBpHExoT5c1sAuENIPPJ481fAuPJfiP+JMhP5c0MBeEY8+nPz8/Pz8/Pz8/P"
    "z8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/P"
    "z8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz881EjIA+gAMgQAARGrh"
    "RGrhDQANAA0AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAHEB3dxxhQEAdd3dDUC9NIDIuMiAtIEFtc3RyYWQgQ29uc3VtZXIgRWxlY3Ryb25pY3MgcGxjCg0kAAAAAAAA"
    "AAgICQEKAgcbQn8QCE8LAAgICQEKAgcbQn8QCE8LAAgICQEKAgAAMTExMTExMTExMTExMTExMTExMTExMTExMTExMTExMTExMTEx"
    "MTExMTExMTExMTExMTExMTExMTExMTExMTExMTExMTExMTExMTExMTExMTExMTExMTExMTExMTExMTExMTExAWYyMjIyMjIyMjIyMjIy"
    "MjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIy"
    "MjIyMjIyMjIyMjIyMjIDHDMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzM0AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl"
    "5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXl5eXDXJnDWJl/ACAgICAgICAg"
    "ICAgICAgICBDT1BZUklHSFQgKEMpIDE5NzksIERJR0lUQUwgUkVTRUFSQ0ggIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACJYAAF8OAsMFAMXNjJbByT4NzZKWPgrDkpY+IMOS"
    "lsXNmJbhfrfII+XNjJbhw6yWDg3DBQBfDg7DBQDNBQAy7p08yQ4Pw8OWrzLtnRHNncPLlg4Qw8OWDhHDw5YOEsPDlhHNncPflg4T"
    "wwUAzQUAt8kOFMP0lhHNncP5lg4Vw/SWDhbDw5YOF8MFAB7/DiDDBQDNE5eHh4eHIe+dtjIEAMk6750yBADJ/mHY/nvQ5l/JOqud"
    "t8qWlzrvnbc+AMS9lhGsnc3LlsqWlzq7nT0yzJ0RrJ3N+ZbClpcRB5YhgAAGgM1CmiG6nTYAIzURrJ3N2pbKlpc67523xL2WIQiW"
    "zayWzcKXyqeXzd2Xw4KZzd2XzRqXDgoRBpbNBQDNKZchB5ZGI3i3yrqXfs0wl3cFw6uXdyEIliKIlskOC80FALfIDgHNBQC3yQ4Z"
    "wwUAEYAADhrDBQAhq51+t8g2AK/NvZYRrJ3N75Y6753DvZYRKJkhAJ4GBhq+ws+ZEyMFwv2Xyc2YliqKln7+IMoimLfKIpjlzYyW"
    "4SPDD5g+P82Mls2Yls3dl8OCmRq3yP4g2gmYyP49yP5fyP4uyP46yP47yP48yP4+yMkat8j+IMATw0+YhW/QJMk+ACHNnc1ZmOXl"
    "rzLwnSqIluvNT5jrIoqW6+Eat8qJmN5ARxMa/jrKkJgbOu+dd8OWmHgy8J1wEwYIzTCYyrmYI/4qwqmYNj/Dq5h3EwXCmJjNMJjK"
    "wJgTw6+YIzYgBcK5mAYD/i7C6ZgTzTCYyumYI/4qwtmYNj/D25h3EwXCyJjNMJjK8JgTw9+YIzYgBcLpmAYDIzYABcLymOsiiJbh"
    "AQsAI37+P8IJmQQNwgGZeLfJRElSIEVSQSBUWVBFU0FWRVJFTiBVU0VS0BYEAAAAIRCZDgB5/gbQEc6dBgQavsJPmRMjBcI8mRr+"
    "IMJUmXnJIwXCT5kMwzOZrzIHljGrncV5Hx8fH+YPX80Vl824ljKrncF55g8y753NvZY6B5a3wpiZMaudzZiWzdCXxkHNjJY+Ps2M"
    "ls05lxGAAM3Yl83QlzLvnc1emMQJmDrwnbfCpZzNLpkhwZlfFgAZGX4jZm/pd5ofm12brZsQnI6cpZwh83YiAJYhAJbpAd+Zw6eW"
    "UkVBRCBFUlJPUgAB8JnDp5ZOTyBGSUxFAM1emDrwnbfCCZghzp0BCwB+/iDKM5oj1jD+CtIJmFd45uDCCZh4BwcHgNoJmIDaCZiC"
    "2gmYRw3CCJrJfv4gwgmYIw3CM5p4yQYDfhIjEwXCQprJIYAAgc1ZmH7JrzLNnTrwnbfIPSHvnb7Iw72WOvCdt8g9Ie+dvsg6753D"
    "vZbNXpjNVJohzp1+/iDCj5oGCzY/IwXCiJoeANXN6ZbM6pnKG5s67p0PDw/mYE8+Cs1LmhfaD5vRexzV5gP1wsyazZiWxc3Ql8HG"
    "Qc2Slj46zZKWw9SazaKWPjrNkpbNopYGAXjNS5rmf/4gwvma8fX+A8L3mj4JzUua5n/+IMoOmz4gzZKWBHj+DNIOm/4JwtmazaKW"
    "w9ma8c3Cl8Ibm83klsOYmtHDhp3NXpj+C8JCmwFSm82nls05lyEHljXCgpkjfv5ZwoKZIyKIls1UmhHNnc3vljzM6pnDhp1BTEwg"
    "KFkvTik/AM1emMIJmM1Ums3Qlsqnm82YliHxnTb/IfGdfv6A2oeb5c3+luHCoJuvdzQhgADNWZh+/hrKhp3NjJbNwpfChp3DdJs9"
    "yoadzdmZzWaawwmYzfiZ9c1emMIJmM1UmhHNndXN75bRzQmXyvubrzLtnfFvJgApEQABfLXK8Zsr5SGAABnlzdiXEc2dzQSX0eHC"
    "+5vD1JsRzZ3N2pY8wgGcAQeczaeWzdWXw4adTk8gU1BBQ0UAzV6YwgmYOvCd9c1Ums3plsJ5nCHNnRHdnQYQzUKaKoiW681PmP49"
    "yj+c/l/Cc5zrIyKIls1emMJznPFHIfCdfrfKWZy4cMJznHCvMs2dzemWym2cEc2dzQ6Xw4adzeqZw4adzWaawwmYAYKczaeWw4ad"
    "RklMRSBFWElTVFMAzfiZ/hDSCZhfOs6d/iDKCZjNFZfDiZ3N9Zc6zp3+IMLEnDrwnbfKiZ09Mu+dzSmXzb2Ww4mdEdadGv4gwgmY"
    "1c1UmtEhg53NQJrN0JbKa50hAAHl683YlxHNnc35lsIBneERgAAZEQCWfZN8mtJxncPhnOE9wnGdzWaazV6YIfCd5X4yzZ0+EM1g"
    "mOF+Mt2drzLtnRFcACHNnQYhzUKaIQiWfrfKPp3+IMo+nSPDMJ0GABGBAH4St8pPnQQjE8NDnXgygADNmJbN1ZfNGpfNAAExq53N"
    "KZfNvZbDgpnNZprDCZgBep3Np5bDhp1CQUQgTE9BRABDT03NZprNXpg6zp3WICHwnbbCCZjDgpkAAAAAAAAAAAAAAAAAAAAAAAAk"
    "JCQgICAgIFNVQgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAANAWBAAAAMMRnpmepZ6rnrGe6yJDoet7MtarIQAAIkWhOSIPoTFBoa8y4Ksy3qshdKvlef4p0EshR55fFgAZGV4jVipD"
    "oevpA6zIoJCfzqASrA+s1KDtoPOg+KDhn/6gfqqDqkWqnKqlqquqyKrXquCq5qrsqvWq/qoEqwqrEassoxerHasmqy2rQatHq02r"
    "DqpTqwShBKGbqyHKns3lnv4DygAAySHVnsO0niHhnsO0niHcns3lnsMAAEJkb3MgRXJyIE9uICA6ICRCYWQgU2VjdG9yJFNlbGVj"
    "dCRGaWxlIFIvTyTlzcmfOkKhxkEyxp4Bup7N05/BzdOfIQ6hfjYAt8DDCazN+57NFJ/Y9U/NkJ/xyf4NyP4KyP4JyP4IyP4gyToO"
    "obfCRZ/NBqzmAcjNCaz+E8JCn80JrP4DygAAr8kyDqE+Ack6CqG3wmKfxc0qn8HFzQyswcU6DaG3xA+swXkhDKH+f8g0/iDQNX63"
    "yHn+CMJ5nzXJ/grANgDJec0Un9KQn/UOXs1In/H2QE95/gnCSJ8OIM1InzoMoeYHwpafyc2snw4gzQysDgjDDKwOI81In83JnzoM"
    "oSELob7QDiDNSJ/DuZ8ODc1Inw4Kw0ifCv4kyAPFT82Qn8HD0586DKEyC6EqQ6FOI+UGAMXlzfue5n/hwf4NysGg/grKwaD+CMIW"
    "oHi3yu+fBToMoTIKocNwoP5/wiagPgjDB6B+BSvDqaD+BcI3oMXlzcmfrzILocPxn/4Qwkig5SENoT4Blnfhw++f/hjCX6DhOguh"
    "IQyhvtLhnzXNpJ/DTqD+FcJroM2xn+HD4Z/+EsKmoMXNsZ/B4eXFeLfKiqAjTgXF5c1/n+HBw3ig5ToKobfK8Z8hDKGWMgqhzaSf"
    "IQqhNcKZoMPxnyN3BMXlT81/n+HBfv4DeMK9oP4BygAAudrvn+FwDg3DSJ/NBp/DAaHNFazDAaF5PMrgoDzKBqzDDKzNBqy3ypGr"
    "zQmswwGhOgMAwwGhIQMAccnrTUTD05/NI58yRaHJPgHDAaEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAIQueXiNW6+kMDcgadxMjw1ChOkKhT80brHy1yF4jViMis6sjIyK1qyMjIrerIyPrItCrIbmr"
    "DgjNT6Equ6vrIcGrDg/NT6Eqxqt8Id2rNv+3yp2hNgA+/7fJzRisryq1q3cjdyq3q3cjd8nNJ6zDu6HNKqy3yCEJnsNKoSrqqw4C"
    "zeqiIuWrIuyrIeWrTiNGKrerXiNWKrWrfiNmb3mTeJrS+qHlKsGre5VfepxX4SvD5KHlKsGrGdoPonmVeJzaD6Lr4SPD+qHhxdXl"
    "6yrOqxlETc0erNEqtatzI3LRKrercyNywXmTT3iaRyrQq+vNMKxNRMMhrCHDq04646u3Hw3CRaJHPgiWTzriqw3KXKK3F8NTooDJ"
    "KkOhERAAGQk63au3ynGibiYAyQleI1bryc0+ok8GAM1eoiLlq8kq5at9tMk6w6sq5aspPcKQoiLnqzrEq08646uhtW8i5avJKkOh"
    "EQwAGckqQ6ERDwAZ6yERABnJza6ifjLjq+t+MuGrzaaiOsWrpjLiq8nNrqI61av+AsLeoq9POuOrgXfrOuGrd8kMDch8tx9nfR9v"
    "w+uiDoAquauvhiMNwv2iyQwNyCnDBaPFOkKhTyEBAM0Eo8F5tW94tGfJKq2rOkKhT83qon3mAckhratOI0bNC6MirasqyKsj6yqz"
    "q3MjcsnNXqMRCQAZfhfQIQ+ew0qhzR6jyCENnsNKoSq5qzrpq4Vv0CTJKkOhEQ4AGX7JzWmjNgDJzWmj9oB3ySrqq+sqs6t7liN6"
    "nsnNf6PYE3Irc8l7lW96nGfJDv8q7KvrKsyrzZWj0MXN96IqvavrKuyrGcEMysSjvsjNf6PQzSyjyXfJzZyjzeCjDgHNuKHD2qPN"
    "4KPNsqEhsavD46MhuatOI0bDJKwquavrKrGrDoDDT6Eh6qt+I77APMkh//8i6qvJKsir6yrqqyMi6qvNlaPSGaTD/qM66qvmAwYF"
    "hwXCIKQy6au3wMXNw6HN1KPBw56jeeYHPF9XeQ8PD+YfT3iHh4eHh7FPeA8PD+YfRyq/qwl+Bx3CVqTJ1c01pOb+wbEPFcJkpHfJ"
    "zV6jERAAGcUOEdENyNU63au3yoikxeVOBgDDjqQNxU4jRuV5sMqdpCrGq32RfJjUXKThI8HDdaQqxqsOA83qoiNETSq/qzYAIwt4"
    "scKxpCrKq+sqv6tzI3LNoaEqs6s2AyM2AM3+ow7/zQWkzfWjyM1eoz7lvsrSpDpBob7C9qQjftYkwvakPTJFoQ4BzWukzYyjw9Kk"
    "OtSrwwGhxfU6xasvR3mgT/GgkeYfwck+/zLUqyHYq3EqQ6Ei2avN/qPNoaEOAM0FpM31o8qUpSrZq+sa/uXKSqXVzX+j0dKUpc1e"
    "ozrYq08GAHm3yoOlGv4/ynyleP4Nynyl/gwaynOlluZ/wi2lw3ylxU7NB6XBwi2lEyMEDcNTpTrqq+YDMkWhIdSrfhfQr3fJzf6j"
    "Pv/DAaHNVKMODM0Ypc31o8jNRKPNXqM25Q4AzWukzcajzS2lw6SlUFl5sMrRpQvVxc01pB/S7KXB0SrGq3uVepzS9KUTxdVCS801"
    "pB/S7KXRwcPApRc8zWSk4dHJebDCwKUhAADJDgAeINUGACpDoQnrzV6jwc1Poc3DocPGo81Uow4MzRilKkOhfhEQABl3zfWjyM1E"
    "ow4QHgzNAabNLaXDJ6YODM0Ypc31o8gOAB4MzQGmzS2lw0CmDg/NGKXN9aPIzaaifvXlzV6j6ypDoQ4g1c1Poc14o9EhDAAZTiEP"
    "ABlG4fF3eb54youmPgDai6Y+gCpDoREPABl3yX4jtivAGncTIxp3GyvJrzJFoTLqqzLrq80eo8DNaaPmgMAOD80Ypc31o8gBEADN"
    "XqMJ6ypDoQkOEDrdq7fK6KZ+txrC26Z3t8Lhpn4SvsIfp8P9ps2UpuvNlKbrGr7CH6cTIxq+wh+nDRMjDcLNpgHs/wnrCRq+2hen"
    "dwEDAAnrCX4SPv8y0qvDEKYhRaE1yc1UoypDoeUhrKsiQ6EOAc0Ypc31o+EiQ6HI6yEPABkOEa93Iw3CRqchDQAZd82Mo839pcN4"
    "o68y0qvNoqbN9aPIKkOhAQwACX485h93yoOnRzrFq6Ah0qumyo6nw6ynAQIACTR+5g/KtqcOD80Ypc31o8KspzrTqzzKtqfNJKfN"
    "9aPKtqfDr6fNWqbNu6KvwwGhzQWhw3ijPgEy1as+/zLTq827ojrjqyHhq77a5qf+gML7p81ap68y46s6RaG3wvunzXeizYSiyvun"
    "zYqizdGhzbKhw9KiwwWhPgEy1as+ADLTq81UoypDoc1Ho827ojrjq/6A0gWhzXeizYSiDgDCbqjNPqIy16sBAAC3yjuoTwvNXqJE"
    "Tc2+pX20wkioPgLDAaEi5avrKkOhARAACTrdq7c616vKZKjNZKNzw2yoTwYACQlzI3IOAjpFobfAxc2KojrVqz09wruowcV5PT3C"
    "u6jlKrmrV3cjFPKMqM3goyrnqw4CIuWrxc3RocHNuKEq5asOADrEq0eluCPCmqjhIuWrzdqjzdGhwcXNuKHBOuOrIeGrvtrSqHc0"
    "DgIAACEAlvXNaaPmf3fx/n/CAKk61av+AcIAqc3Sos1apyFFoX63wv6oPTLjqzYAw9KirzLVq8UqQ6HrISEAGX7mf/V+FyN+F+Yf"
    "T34fHx8f5g9H8SNuLC0uBsKLqSEgABl3IQwAGXmWwkepIQ4AGXiW5n/Kf6nF1c2iptHBLgM6RaE8yoSpIQwAGXEhDgAZcM1RpjpF"
    "oTzCf6nBxS4EDMqEqc0kpy4FOkWhPMqEqcGvwwGh5c1pozbA4cF9MkWhw3ijDv/NA6nMwafJDgDNA6nMA6jJ6xlOBgAhDAAZfg/m"
    "gIFPPgCIR34P5g+ARyEOABl+h4eHh/WAR/XhfeG15gHJDgzNGKUqQ6ERIQAZ5XIjciNyzfWjygyqzV6jEQ8AzaWp4eVfeZYjeJ4j"
    "e57aBqpzK3Arcc0tpcPkqeHJKkOhESAAzaWpISEAGXEjcCN3ySqvqzpCoU/N6qLl681ZoeHMR6F9H9gqr6tNRM0LoyKvq8OjpDrW"
    "qyFCob7Id8Mhqj7/Mt6rKkOhfuYfPTLWq/4e0nWqOkKhMt+rfjLgq+bgd81FqjpBoSpDobZ3yT4iwwGhIQAAIq2rIq+rrzJCoSGA"
    "ACKxq83ao8Mhqs1yo81RqsNRps1RqsOipg4A637+P8rCqs2mon7+P8Ryo81Rqg4PzRilw+mjKtmrIkOhzVGqzS2lw+mjzVGqzZyl"
    "wwGlzVGqw7ynzVGqw/6nzXKjzVGqwySnzVGqzRamwwGlKq+rwymrOkKhwwGh6yKxq8Paoyq/q8Mpqyqtq8Mpq81Rqs07psMBpSq7"
    "qyJFock61qv+/8I7qzpBocMBoeYfMkGhyc1RqsOTqc1RqsOcqc1RqsPSqSpDoX0vX3wvKq+rpFd9o18qravrIq+rfaNvfKJnIq2r"
    "yTreq7fKkasqQ6E2ADrgq7fKkat3Ot+rMtarzUWqKg+h+SpFoX1Eyc1Rqj4CMtWrDgDNB6nMA6jJ5QAAAACAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

static Bytes base64decode(const char* in) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static int table[256];
    static bool init = [] { for (int i = 0; i < 256; i++) table[i] = -1; for (int i = 0; i < (int)chars.size(); i++) table[(unsigned char)chars[i]] = i; return true; }();
    (void)init;
    Bytes out;
    int val = 0, bits = -8;
    for (const char* p = in; *p; ++p) {
        char c = *p;
        if (c == '=') break;
        int d = table[(unsigned char)c];
        if (d < 0) continue;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) { out.push_back((uint8_t)((val >> bits) & 0xff)); bits -= 8; }
    }
    return out;
}

Bytes cpm22SystemBytes() { return base64decode(CPM22_BASE64); }
int cpm22SystemSize() { return 9216; }

} // namespace cpcse
