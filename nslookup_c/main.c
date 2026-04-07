/*
 * ============================================================================
 * MX Record Email Validator
 * ============================================================================
 *
 * PURPOSE:
 *   Checks if a domain has valid MX (Mail Exchange) records before attempting
 *   to send emails. This prevents sending emails to non-existent domains and
 *   validates that the recipient's mail server can receive emails.
 *
 * WHAT ARE MX RECORDS?
 *   MX records are DNS entries that specify which mail servers are responsible
 *   for receiving emails for a domain. Example:
 *
 *   $ nslookup -type=mx gmail.com
 *   gmail.com  mail exchanger = 5 gmail-smtp-in.l.google.com
 *   gmail.com  mail exchanger = 10 alt1.gmail-smtp-in.l.google.com
 *
 * WHY CHECK MX RECORDS BEFORE SENDING EMAIL?
 *   1. Validate domain exists in DNS
 *   2. Verify domain is configured to receive emails
 *   3. Prevent email bounces to non-existent domains
 *   4. Identify which mail servers handle email for the domain
 *   5. Save resources by filtering invalid recipients early
 *
 * COMPILE:
 *   gcc -o nslookup.c.o main.c -lresolv
 *
 * USAGE:
 *   ./nslookup.co <domain_or_email>
 *
 * EXAMPLES:
 *   # Check a domain
 *   ./nslookup.co gmail.com
 *
 *   # Check an email address
 *   ./nslookup.co user@gmail.com
 *
 *   # Check from the original comments
 *   ./nslookup.co qwen.com
 *
 * EXPECTED OUTPUT (valid domain):
 *   Checking MX records for domain: gmail.com
 *   ----------------------------------------
 *   Result: VALID - Found 5 MX record(s)
 *
 *   Mail Exchange Servers:
 *     [1] Priority: 5, Server: gmail-smtp-in.l.google.com
 *     [2] Priority: 10, Server: alt1.gmail-smtp-in.l.google.com
 *     ...
 *
 * EXPECTED OUTPUT (invalid domain):
 *   Checking MX records for domain: nonexistent.foo
 *   ----------------------------------------
 *   Domain 'nonexistent.foo' does not exist
 *   Result: INVALID - Cannot receive emails
 *
 * RETURN CODES:
 *   0 - Domain has valid MX records (can receive emails)
 *   1 - Domain is invalid or cannot receive emails
 *
 * INTEGRATION EXAMPLE:
 *   # In your email sending script/application:
 *   if ./nslookup.co "$recipient_domain"; then
 *       echo "Domain can receive emails, proceeding..."
 *       # Send email here
 *   else
 *       echo "Invalid domain, skipping..."
 *       # Log error or skip
 *   fi
 *
 * NOTES:
 *   - SMTP UNREACHABLE is common (many ISPs block port 25 outbound)
 *   - Domains without MX records might still receive email via A records
 *     (per RFC 5321), but this tool checks for proper MX configuration
 *   - Lower MX priority number = higher preference (attempted first)
 *
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <netdb.h>
#include <errno.h>
#include <unistd.h>

#define MAX_MX_RECORDS 20
#define MAX_DOMAIN_LEN 256
#define RESPONSE_SIZE 4096

/* MX record structure */
typedef struct {
    int priority;
    char exchange[MAX_DOMAIN_LEN];
} MXRecord;

/*
 * Check MX records for a domain using res_query and ns_initparse/ns_parserr
 * Returns:
 *    1 : Error occurred
 *    0 : No MX records found
 *    0 : Number of MX records found
 */
int check_mx_records(const char *domain, MXRecord *mx_records, int max_records) {
    unsigned char response[RESPONSE_SIZE];
    int response_len;

    /* Initialize resolver */
    res_init();

    /* Build DNS query for MX records */
    response_len = res_query(domain, C_IN, T_MX, response, RESPONSE_SIZE);
    if (response_len < 0) {
        if (h_errno == HOST_NOT_FOUND) {
            fprintf(stderr, "Domain '%s' does not exist\n", domain);
        } else if (h_errno == TRY_AGAIN) {
            fprintf(stderr, "DNS server temporarily unavailable\n");
        } else if (h_errno == NO_RECOVERY) {
            fprintf(stderr, "Non-recoverable DNS error\n");
        } else if (h_errno == NO_DATA) {
            printf("Domain '%s' exists but has no MX records\n", domain);
        }
        return -1;
    }

    /* Use ns_initparse to parse the response */
    ns_msg handle;
    if (ns_initparse(response, response_len, &handle) < 0) {
        fprintf(stderr, "Failed to parse DNS response\n");
        return -1;
    }

    int mx_count = 0;
    int ancount = ns_msg_count(handle, ns_s_an);

    for (int i = 0; i < ancount && mx_count < max_records; i++) {
        ns_rr rr;

        if (ns_parserr(&handle, ns_s_an, i, &rr) < 0) {
            continue;
        }

        /* Check if this is an MX record */
        if (ns_rr_type(rr) == ns_t_mx) {
            const unsigned char *rdata = ns_rr_rdata(rr);

            /* MX record format: 2 bytes priority + domain name */
            if (ns_rr_rdlen(rr) >= 3) {
                uint16_t priority = (rdata[0] << 8) | rdata[1];

                /* Expand the domain name */
                char exchange[MAX_DOMAIN_LEN];
                int expanded = ns_name_uncompress(response, 
                                                 response + response_len,
                                                 rdata + 2,
                                                 exchange,
                                                 MAX_DOMAIN_LEN);

                if (expanded > 0) {
                    mx_records[mx_count].priority = priority;
                    strncpy(mx_records[mx_count].exchange, exchange, MAX_DOMAIN_LEN - 1);
                    mx_records[mx_count].exchange[MAX_DOMAIN_LEN - 1] = '\0';
                    mx_count++;
                }
            }
        }
    }
    
    /* Sort by priority (simple bubble sort) */
    for (int i = 0; i < mx_count - 1; i++) {
        for (int j = 0; j < mx_count - i - 1; j++) {
            if (mx_records[j].priority > mx_records[j + 1].priority) {
                MXRecord temp = mx_records[j];
                mx_records[j] = mx_records[j + 1];
                mx_records[j + 1] = temp;
            }
        }
    }

    return mx_count;
}

/*
 * Validate email domain by checking MX records
 * Returns 1 if valid, 0 if invalid
 */
int validate_email_domain(const char *domain) {
    MXRecord mx_records[MAX_MX_RECORDS];
    int mx_count;

    printf("Checking MX records for domain: %s\n", domain);
    printf("----------------------------------------\n");

    mx_count = check_mx_records(domain, mx_records, MAX_MX_RECORDS);

    if (mx_count < 0) {
        printf("Result: INVALID - Cannot receive emails\n");
        return 0;
    } else if (mx_count == 0) {
        printf("Result: INVALID - No MX records found\n");
        return 0;
    }

    printf("Result: VALID - Found %d MX record(s)\n", mx_count);
    printf("\nMail Exchange Servers:\n");
    for (int i = 0; i < mx_count; i++) {
        printf("  [%d] Priority: %d, Server: %s\n", 
               i + 1, mx_records[i].priority, mx_records[i].exchange);
    }

    return 1;
}

/*
 * Validate full email address
 * Extracts domain from email and checks MX records
 */
int validate_email_address(const char *email) {
    char domain[MAX_DOMAIN_LEN];
    const char *at_sign;

    /* Find @ symbol */
    at_sign = strchr(email, '@');
    if (!at_sign) {
        fprintf(stderr, "Invalid email format: missing @\n");
        return 0;
    }

    /* Extract domain part */
    strncpy(domain, at_sign + 1, MAX_DOMAIN_LEN - 1);
    domain[MAX_DOMAIN_LEN - 1] = '\0';

    /* Basic domain validation */
    if (strlen(domain) == 0) {
        fprintf(stderr, "Invalid email format: empty domain\n");
        return 0;
    }

    return validate_email_domain(domain);
}

/*
 * Check if mail server is reachable (optional additional validation)
 * Attempts to connect to port 25 (SMTP)
 */
int check_smtp_reachable(const char *hostname) {
    struct addrinfo hints, *res, *p;
    int sockfd = -1;
    int reachable = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, "25", &hints, &res) != 0) {
        fprintf(stderr, "  Warning: Could not resolve %s\n", hostname);
        return 0;
    }

    /* Try to connect to each address */
    for (p = res; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) continue;

        /* Set timeout */
        struct timeval tv;
        tv.tv_sec = 3;
        tv.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == 0) {
            reachable = 1;
            close(sockfd);
            break;
        }

        close(sockfd);
    }

    freeaddrinfo(res);
    return reachable;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <domain_or_email>\n", argv[0]);
        printf("\nExamples:\n");
        printf("  %s gmail.com\n", argv[0]);
        printf("  %s user@gmail.com\n", argv[0]);
        printf("  %s qwen.com\n", argv[0]);
        printf("\nChecks if a domain has valid MX records to receive emails.\n");
        return EXIT_FAILURE;
    }

    const char *input = argv[1];
    int is_valid;

    /* Determine if input is email or domain */
    if (strchr(input, '@')) {
        is_valid = validate_email_address(input);
    } else {
        is_valid = validate_email_domain(input);
    }

    printf("\n");

    /* Optional: Test SMTP connectivity to first MX server */
    if (is_valid) {
        MXRecord mx_records[MAX_MX_RECORDS];
        char domain[MAX_DOMAIN_LEN];

        if (strchr(input, '@')) {
            strncpy(domain, strchr(input, '@') + 1, MAX_DOMAIN_LEN - 1);
            domain[MAX_DOMAIN_LEN - 1] = '\0';
        } else {
            strncpy(domain, input, MAX_DOMAIN_LEN - 1);
            domain[MAX_DOMAIN_LEN - 1] = '\0';
        }

        int mx_count = check_mx_records(domain, mx_records, MAX_MX_RECORDS);
        if (mx_count > 0) {
            printf("Testing SMTP connectivity to primary MX server: %s\n", mx_records[0].exchange);
            if (check_smtp_reachable(mx_records[0].exchange)) {
                printf("SMTP Status: REACHABLE (port 25 open)\n");
            } else {
                printf("SMTP Status: UNREACHABLE (port 25 blocked or server down)\n");
            }
        }
    }

    return is_valid ? EXIT_SUCCESS : EXIT_FAILURE;
}
