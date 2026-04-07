/*
 * ============================================================================
 * MX Record Email Validator (Go Implementation)
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
 * BUILD & RUN:
 *   go build -o nslookup.go.o main.go
 *   ./nslookup.go.o gmail.com
 *
 * USAGE:
 *   go run main.go <domain_or_email>
 *
 * EXAMPLES:
 *   # Check a domain
 *   go run main.go gmail.com
 *
 *   # Check an email address
 *   go run main.go user@gmail.com
 *
 *   # Check specific domain
 *   go run main.go qwen.com
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
 *   Domain 'nonexistent.foo' does not exist or has no MX records
 *   Result: INVALID - Cannot receive emails
 *
 * RETURN CODES:
 *   0 - Domain has valid MX records (can receive emails)
 *   1 - Domain is invalid or cannot receive emails
 *
 * INTEGRATION EXAMPLE:
 *   # In your email sending script/application:
 *   if go run main.go "$recipient_domain"; then
 *       echo "Domain can receive emails, proceeding..."
 *       # Send email here
 *   else
 *       echo "Invalid domain, skipping..."
 *       # Log error or skip
 *   fi
 *
 * NOTES:
 *   - SMTP UNREACHABLE is common (many ISPs block port 25 outbound)
 *   - Go's net.LookupMX() handles DNS resolution automatically
 *   - Lower MX priority number = higher preference (attempted first)
 *
 * ============================================================================
 */

package main

import (
	"fmt"
	"net"
	"os"
	"sort"
	"strings"
	"time"
)

// MXRecord represents a mail exchange server entry
type MXRecord struct {
	Priority uint16
	Host     string
}

// checkMXRecords queries DNS for MX records of a domain
// Returns slice of MX records sorted by priority (lower = higher preference)
func checkMXRecords(domain string) ([]MXRecord, error) {
	// Use Go's built-in DNS lookup
	mxRecords, err := net.LookupMX(domain)
	if err != nil {
		// Check for specific error types
		if dnsErr, ok := err.(*net.DNSError); ok {
			if dnsErr.IsNotFound {
				return nil, fmt.Errorf("domain '%s' does not exist or has no MX records", domain)
			}
			if dnsErr.IsTimeout {
				return nil, fmt.Errorf("DNS query timed out for domain '%s'", domain)
			}
		}
		return nil, fmt.Errorf("DNS lookup failed for '%s': %w", domain, err)
	}

	if len(mxRecords) == 0 {
		return nil, fmt.Errorf("domain '%s' exists but has no MX records", domain)
	}

	// Convert to our MXRecord type
	records := make([]MXRecord, len(mxRecords))
	for i, mx := range mxRecords {
		records[i] = MXRecord{
			Priority: mx.Pref,
			Host:     mx.Host,
		}
	}

	// Sort by priority (Go's net.LookupMX usually returns sorted, but ensure it)
	sort.Slice(records, func(i, j int) bool {
		return records[i].Priority < records[j].Priority
	})

	return records, nil
}

// validateEmailDomain checks if a domain has valid MX records
// Returns true if the domain can receive emails
func validateEmailDomain(domain string) bool {
	fmt.Printf("Checking MX records for domain: %s\n", domain)
	fmt.Println("----------------------------------------")

	records, err := checkMXRecords(domain)
	if err != nil {
		fmt.Printf("Result: INVALID - %v\n", err)
		return false
	}

	fmt.Printf("Result: VALID - Found %d MX record(s)\n", len(records))
	fmt.Println("\nMail Exchange Servers:")
	for i, record := range records {
		fmt.Printf("  [%d] Priority: %d, Server: %s\n",
			i+1, record.Priority, record.Host)
	}

	return true
}

// validateEmailAddress extracts domain from email and validates it
func validateEmailAddress(email string) bool {
	// Find @ symbol
	atIndex := strings.LastIndex(email, "@")
	if atIndex == -1 {
		fmt.Fprintf(os.Stderr, "Invalid email format: missing @\n")
		return false
	}

	// Extract domain part
	domain := email[atIndex+1:]
	if domain == "" {
		fmt.Fprintf(os.Stderr, "Invalid email format: empty domain\n")
		return false
	}

	return validateEmailDomain(domain)
}

// checkSMTPReachable tests if the mail server is reachable on port 25 (SMTP)
// Optional additional validation - note that many networks block port 25
func checkSMTPReachable(hostname string) bool {
	// MX hostnames often end with a dot, remove it if present
	host := strings.TrimSuffix(hostname, ".")

	// Default SMTP port
	port := "25"
	address := host + ":" + port

	// Try to connect with timeout
	conn, err := net.DialTimeout("tcp", address, 3*time.Second)
	if err != nil {
		return false
	}
	defer conn.Close()

	return true
}

func printUsage() {
	fmt.Println("Usage: go run main.go <domain_or_email>")
	fmt.Println()
	fmt.Println("Examples:")
	fmt.Println("  go run main.go gmail.com")
	fmt.Println("  go run main.go user@gmail.com")
	fmt.Println("  go run main.go qwen.com")
	fmt.Println()
	fmt.Println("Checks if a domain has valid MX records to receive emails.")
}

func main() {
	if len(os.Args) < 2 {
		printUsage()
		os.Exit(1)
	}

	input := os.Args[1]
	var isValid bool

	// Determine if input is email or domain
	if strings.Contains(input, "@") {
		isValid = validateEmailAddress(input)
	} else {
		isValid = validateEmailDomain(input)
	}

	fmt.Println()

	// Optional: Test SMTP connectivity to first MX server
	if isValid {
		var domain string
		if strings.Contains(input, "@") {
			atIndex := strings.LastIndex(input, "@")
			domain = input[atIndex+1:]
		} else {
			domain = input
		}

		records, err := checkMXRecords(domain)
		if err == nil && len(records) > 0 {
			fmt.Printf("Testing SMTP connectivity to primary MX server: %s\n", records[0].Host)
			if checkSMTPReachable(records[0].Host) {
				fmt.Println("SMTP Status: REACHABLE (port 25 open)")
			} else {
				fmt.Println("SMTP Status: UNREACHABLE (port 25 blocked or server down)")
			}
		}
	}

	if !isValid {
		os.Exit(1)
	}
}
