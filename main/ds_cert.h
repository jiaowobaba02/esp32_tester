#ifndef DS_CERT_H
#define DS_CERT_H

/* DeepSeek api.deepseek.com 信任链 (中间 + 根):
 * 1. TrustAsia DV TLS RSA CA 2025 (中间, 至 2035-01)
 * 2. DigiCert Global Root G2 (根, 至 2038-01)
 * 策略: 主用 esp_crt_bundle (系统证书包), 仅当其验证失败时回退本链,
 * 覆盖服务器不下发中间证书的节点. 故意不固定叶子证书 (api.deepseek.com
 * 每季度轮换, 固定叶子会在轮换后导致验证失败). 中间/根轮换后需重新导出. */
static const char s_ds_cert[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIFnjCCBIagAwIBAgIQCSYyO0lk42hGFRLe8aXVLDANBgkqhkiG9w0BAQsFADBh\n"
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
    "MjAeFw0yNTAxMDgwMDAwMDBaFw0zNTAxMDcyMzU5NTlaMFsxCzAJBgNVBAYTAkNO\n"
    "MSUwIwYDVQQKExxUcnVzdEFzaWEgVGVjaG5vbG9naWVzLCBJbmMuMSUwIwYDVQQD\n"
    "ExxUcnVzdEFzaWEgRFYgVExTIFJTQSBDQSAyMDI1MIICIjANBgkqhkiG9w0BAQEF\n"
    "AAOCAg8AMIICCgKCAgEA0fuEmuBIsN6ZZVq+gRobMorOGIilTCIfQrxNpR8FUZ9R\n"
    "/GfbiekbiIKphQXEZ7N1uBnn6tXUuZ32zl6jPkZpHzN/Bmgk1BWSIzVc0npMzrWq\n"
    "/hrbk5+KddXJdsNpeG1+Q8lc8uVMBrztnxaPb7Rh7yQCsMrcO4hgVaqLJWkVvEfW\n"
    "ULtoCHQnNaj4IroG6VxQf1oArQ8bPbwpI02lieSahRa78FQuXdoGVeQcrkhtVjZs\n"
    "ON98vq5fPWZX2LFv7e5J6P9IHbzvOl8yyQjv+2/IOwhNSkaXX3bI+//bqF9XW/p7\n"
    "+gsUmHiK5YsvLjmXcvDmoDEGrXMzgX31Zl2nJ+umpRbLjwP8rxYIUsKoEwEdFoto\n"
    "Aid59UEBJyw/GibwXQ5xTyKD/N6C8SFkr1+myOo4oe1UB+YgvRu6qSxIABo5kYdX\n"
    "FodLP4IgoVJdeUFs1Usa6bxYEO6EgMf5lCWt9hGZszvXYZwvyZGq3ogNXM7eKyi2\n"
    "20WzJXYMmi9TYFq2Fa95aZe4wki6YhDhhOO1g0sjITGVaB73G+JOCI9yJhv6+REN\n"
    "D40ZpboUHE8JNgMVWbG1isAMVCXqiADgXtuC+tmJWPEH9cR6OuJLEpwOzPfgAbnn\n"
    "2MRu7Tsdr8jPjTPbD0FxblX1ydW3RG30vwLF5lkTTRkHG9epMgpPMdYP7nY/08MC\n"
    "AwEAAaOCAVYwggFSMBIGA1UdEwEB/wQIMAYBAf8CAQAwHQYDVR0OBBYEFLQSKKW0\n"
    "wB2fKXFpPNkRlkp1aVDAMB8GA1UdIwQYMBaAFE4iVCAYlebjbuYP+vq5Eu0GF485\n"
    "MA4GA1UdDwEB/wQEAwIBhjAdBgNVHSUEFjAUBggrBgEFBQcDAQYIKwYBBQUHAwIw\n"
    "dgYIKwYBBQUHAQEEajBoMCQGCCsGAQUFBzABhhhodHRwOi8vb2NzcC5kaWdpY2Vy\n"
    "dC5jb20wQAYIKwYBBQUHMAKGNGh0dHA6Ly9jYWNlcnRzLmRpZ2ljZXJ0LmNvbS9E\n"
    "aWdpQ2VydEdsb2JhbFJvb3RHMi5jcnQwQgYDVR0fBDswOTA3oDWgM4YxaHR0cDov\n"
    "L2NybDMuZGlnaWNlcnQuY29tL0RpZ2lDZXJ0R2xvYmFsUm9vdEcyLmNybDARBgNV\n"
    "HSAECjAIMAYGBFUdIAAwDQYJKoZIhvcNAQELBQADggEBAJ4a3svh316GY2+Z7EYx\n"
    "mBIsOwjJSnyoEfzx2T699ctLLrvuzS79Mg3pPjxSLlUgyM8UzrFc5tgVU3dZ1sFQ\n"
    "I4RM+ysJdvIAX/7Yx1QbooVdKhkdi9X7QN7yVkjqwM3fY3WfQkRTzhIkM7mYIQbR\n"
    "r+y2Vkju61BLqh7OCRpPMiudjEpP1kEtRyGs2g0aQpEIqKBzxgitCXSayO1hoO6/\n"
    "71ts801OzYlqYW9OQQQ2GCJyFbD6XHDjdpn+bWUxTKWaMY0qedSCbHE3Kl2QEF0C\n"
    "ynZ7SbC03yR+gKZQDeTXrNP1kk5Qhe7jSXgw+nhbspe0q/M1ZcNCz+sPxeOwdCcC\n"
    "gJE=\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
    "MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
    "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
    "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
    "2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
    "1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
    "q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
    "tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
    "vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
    "BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
    "5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
    "1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
    "NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
    "Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
    "8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
    "pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
    "MrY=\n"
    "-----END CERTIFICATE-----\n"
    "\n"
    ;

#endif
