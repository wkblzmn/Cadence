import type { Metadata } from "next";
import { Geist, Geist_Mono } from "next/font/google";
import "./globals.css";

const geistSans = Geist({
  variable: "--font-geist-sans",
  subsets: ["latin"],
});

const geistMono = Geist_Mono({
  variable: "--font-geist-mono",
  subsets: ["latin"],
});

export const metadata: Metadata = {
  title: "Cadence",
  description: "Focus and environment tracking for the Cadence desk hub.",
};

export default function RootLayout({ children }: LayoutProps<"/">) {
  return (
    <html
      lang="en"
      className={`${geistSans.variable} ${geistMono.variable} h-full antialiased`}
    >
      <body className="min-h-full flex flex-col">
        {/* Applies the stored theme before first paint. Without this the page
            renders in the OS theme and then snaps to the chosen one — a flash
            of the wrong colours on every navigation. */}
        <script
          dangerouslySetInnerHTML={{
            __html:
              "(function(){try{var t=localStorage.getItem('cadence-theme');" +
              "if(t==='light'||t==='dark')document.documentElement.dataset.theme=t;}catch(e){}})();",
          }}
        />
        {children}
      </body>
    </html>
  );
}
