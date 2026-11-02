import { expect, test } from "@playwright/test";

const shot = process.env.DOCS_ASSETS;

test("full journey from capture to match, with visual checks", async ({ page }) => {
  const save = async (name: string) => {
    await expect(page).toHaveScreenshot(`${name}.png`, { fullPage: true });
    if (shot) await page.screenshot({ path: `${shot}/${name}.png`, fullPage: true });
  };
  await page.goto("/");
  await expect(page.getByText("The tray is empty")).toBeVisible();

  await page.getByRole("button", { name: "Load sample gallery" }).click();
  await expect(page.getByRole("button", { name: /mock-1002-2/ })).toBeVisible({ timeout: 90_000 });

  // Scan: conversion steps and overlay.
  await expect(page.getByText("Compress to WSQ")).toBeVisible();
  await expect(page.getByText("Extract MINEX")).toBeVisible();
  expect(await page.locator(".marker").count()).toBeGreaterThan(5);
  await page.addStyleTag({ content: ".steps .quiet { visibility: hidden; }" }); // timings vary between runs
  await save("scan");

  // Identify an enrolled person with two rescanned fingers.
  await page.getByRole("button", { name: "Identify" }).click();
  await page.getByRole("checkbox", { name: "mock-1002-2.bmp" }).check();
  await page.getByRole("checkbox", { name: "mock-1007-2.bmp" }).check();
  await page.getByRole("button", { name: "Search gallery" }).click();
  await expect(page.getByText("Identified as customer-1001")).toBeVisible({ timeout: 30_000 });

  // A stranger is not identified.
  await page.getByRole("checkbox", { name: "mock-1002-2.bmp" }).uncheck();
  await page.getByRole("checkbox", { name: "mock-1007-2.bmp" }).uncheck();
  await page.getByRole("checkbox", { name: "mock-9000-1.bmp" }).check();
  await page.getByRole("button", { name: "Search gallery" }).click();
  await expect(page.getByText("No match in the gallery")).toBeVisible({ timeout: 30_000 });

  // Compare the same finger against a stranger's.
  await page.getByRole("button", { name: "Compare", exact: true }).click();
  await page.getByLabel("First print").selectOption({ label: "mock-1002-2.bmp" });
  await page.getByLabel("Second print").selectOption({ label: "mock-9000-1.bmp" });
  await page.getByRole("button", { name: "Compare prints" }).click();
  await expect(page.getByText("Different fingers")).toBeVisible();

  // Rescan and compare: lines connect the matched minutiae.
  await page.getByRole("button", { name: /mock-1002-2/ }).click();
  await page.getByRole("button", { name: "Rescan selected finger" }).click();
  await expect(page.getByRole("button", { name: /mock-1002-3/ })).toBeVisible({ timeout: 30_000 });
  await page.getByRole("button", { name: "Compare", exact: true }).click();
  await page.getByLabel("First print").selectOption({ label: "mock-1002-2.bmp" });
  await page.getByLabel("Second print").selectOption({ label: "mock-1002-3.bmp" });
  await page.getByRole("button", { name: "Compare prints" }).click();
  await expect(page.getByText("Same finger")).toBeVisible();
  expect(await page.locator(".pair").count()).toBeGreaterThan(5);
  await save("compare");

  // Cross-match: the two scans of one finger light up off the diagonal.
  await page.getByRole("button", { name: "Cross-match", exact: true }).click();
  await page.getByRole("button", { name: /Cross-match 4 prints/ }).click();
  await expect(page.locator(".heat td.hit")).toHaveCount(6); // 4 diagonal + 1 pair both ways
  await save("crossmatch");

  // Enrol a new customer through the screen.
  await page.getByRole("button", { name: "Enrol", exact: true }).click();
  await page.getByLabel("Customer ID").fill("customer-new");
  await page.getByRole("checkbox", { name: "mock-9000-1.bmp" }).check();
  await page.getByRole("button", { name: "Enrol customer" }).click();
  await expect(page.getByRole("status").getByText("Enrolled customer-new with 1 finger.")).toBeVisible();

  // Performance reflects it, and states what is not yet proven.
  await page.getByRole("button", { name: "Performance" }).click();
  await expect(page.getByText("design target, not a measured result")).toBeVisible();
  await expect(page.getByRole("row", { name: /Customers 7/ })).toBeVisible();
});

test("uploads: template works, garbage gets a clear error", async ({ page, request }) => {
  await page.goto("/");
  const res = await request.post("/v1/demo/synthetic", { data: { finger_id: 77, impression: 0 } });
  const { file } = await res.json();
  const inspected = await (await request.post("/v1/inspect", { data: { file } })).json();

  await page.getByTestId("upload").setInputFiles([
    { name: "scan.wsq", mimeType: "application/octet-stream", buffer: Buffer.from(inspected.wsq, "base64") },
    { name: "print.ansi", mimeType: "application/octet-stream", buffer: Buffer.from(inspected.template_ansi, "base64") },
    { name: "junk.bmp", mimeType: "application/octet-stream", buffer: Buffer.from("not an image") },
  ]);
  await expect(page.getByRole("button", { name: /scan\.wsq/ })).toBeVisible({ timeout: 30_000 });
  await expect(page.getByRole("button", { name: /print\.ansi/ })).toBeVisible();
  await expect(page.getByRole("alert")).toContainText("junk.bmp");

  await page.getByRole("button", { name: /print\.ansi/ }).click();
  await expect(page.getByText("already a template")).toBeVisible();
});
